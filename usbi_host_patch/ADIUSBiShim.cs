// ============================================================================
//  ADIUSBi 兼容实现（EP32-S3 USBi 加速版）
//
//  作用：替换 SigmaStudio 安装目录里的 ADIUSBi.dll。SigmaStudio 通过它跟 USBi
//        设备通信；原版为了让 FX2 固件有时间处理，把每个 USB 控制传输限制在
//        64 字节，而且每次读写都额外发 0xB5/0xB6 状态轮询（每次 ~4ms）。
//
//  本实现针对 ESP32-S3 的 USBi（固件同步完成 I2C 操作）做了两处优化：
//    1) 不轮询 0xB5/0xB6 —— 控制传输结束即代表 I2C 已完成
//    2) 把每次 USB 传输的数据量提高到 4096 字节 —— 下载程序时传输次数从
//       316 次降到 5 次
//  实测（400kHz I2C）：4 字节读 16.2ms → 8.0ms；4 字节写 8.1ms → 4.0ms；
//  4KB 程序块写 170ms（原来 64B/次要 316 次）。
//
//  注意：这是可选的"主机端加速"。原版 ADIUSBi.dll 会被备份成 ADIUSBi.dll.orig，
//        运行 uninstall.ps1 可随时还原。仅对本项目的 ESP32-S3 USBi 有效，
//        接真实 USBi 仿真器时请还原原版 DLL。
// ============================================================================

using System;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using CyUSB;

// ⚠️ 以下三个类型在原版 ADIUSBi.dll 里位于**全局命名空间**，名字/成员必须一致
public enum TransferStatus
{
    None = 0,
    Success = 1,
    Writing = 2,
    Failed = 3,
    Error = 4,
    Exception = 5,
}

public enum CommsFormat
{
    I2C = 0,
    SPI = 1,
}

public delegate void PnPCallback(bool connected);

namespace Analog.ADIUSBi
{
    public class ADIUSBi : IDisposable
    {
        private const int VID = 0x0456;
        private const int PID = 0x7031;
        private const int MAX_XFER = 4096;      // 与固件 s_out_buf/s_in_buf 一致

        // USBi 协议命令
        private const byte CMD_WRITE = 0xB2;
        private const byte CMD_WRITE_READ = 0xB3;
        private const byte CMD_READ = 0xB4;
        private const byte CMD_CHECK_WRITE = 0xB5;
        private const byte CMD_CHECK_READ = 0xB6;
        private const byte CMD_RESET = 0xB7;
        private const byte CMD_POWERBOARD = 0xB8;
        private const byte CMD_LED = 0xB9;
        private const byte CMD_RESET_TOGGLE = 0xBA;

        private static readonly ADIUSBi _singleton = new ADIUSBi();

        /// <summary>SigmaStudio 用的单例入口</summary>
        public static ADIUSBi USBi
        {
            get { return _singleton; }
        }

        private readonly object _lock = new object();
        private USBDeviceList _list;
        private CyUSBDevice _dev;
        private CyControlEndPoint _ep;
        private int _maxDataBytes = MAX_XFER;
        private int _splitDelayMs;
        private PnPCallback _pnpEvent;

        public event PnPCallback PnPCallbackEvent
        {
            add { _pnpEvent += value; }
            remove { _pnpEvent -= value; }
        }

        public bool IsDeivceConnected
        {
            get
            {
                lock (_lock)
                {
                    return _ep != null;
                }
            }
        }

        public ADIUSBi()
        {
            InitDeviceList();
        }

        // ------------------------------------------------------------------
        //  设备发现
        // ------------------------------------------------------------------
        private void InitDeviceList()
        {
            try
            {
                _list = new USBDeviceList((byte)CyConst.DEVICES_CYUSB,
                                          new App_PnP_Callback(OnPnP));
            }
            catch (Exception)
            {
                _list = null;
            }
            initDevice();
        }

        private void OnPnP(IntPtr pnpEvent, IntPtr hRemovedDevice)
        {
            try
            {
                if (pnpEvent == (IntPtr)CyConst.DBT_DEVICEREMOVECOMPLETE)
                {
                    if (_list != null)
                    {
                        try { _list.Remove(hRemovedDevice); } catch (Exception) { }
                    }
                }
                else if (pnpEvent == (IntPtr)CyConst.DBT_DEVICEARRIVAL)
                {
                    if (_list != null)
                    {
                        try { _list.Add(); } catch (Exception) { }
                    }
                }
            }
            catch (Exception) { }

            initDevice();

            PnPCallback cb = _pnpEvent;
            if (cb != null)
            {
                try { cb(IsDeivceConnected); } catch (Exception) { }
            }
        }

        public void initDevice()
        {
            lock (_lock)
            {
                _ep = null;
                _dev = null;
                if (_list == null)
                {
                    return;
                }
                try
                {
                    _dev = _list[(int)VID, (int)PID] as CyUSBDevice;
                }
                catch (Exception)
                {
                    _dev = null;
                }
                if (_dev == null)
                {
                    return;
                }
                _ep = _dev.ControlEndPt;
                if (_ep != null)
                {
                    _ep.Target = 0;             // CyConst.TGT_DEVICE
                    _ep.ReqType = 0x40;         // vendor request
                    _ep.TimeOut = 1000;         // 与原 ADIUSBi 一致
                }
            }
        }

        // ------------------------------------------------------------------
        //  控制传输
        // ------------------------------------------------------------------
        private bool CtrlOut(byte req, ushort value, ushort index, byte[] data)
        {
            CyControlEndPoint ep = _ep;
            if (ep == null || data == null)
            {
                return false;
            }
            byte[] buf = new byte[data.Length];
            Array.Copy(data, buf, data.Length);
            int len = data.Length;
            ep.Direction = 0;               // 0 = OUT（与 CyUSB 示例/ADIUSBi 的用法一致）
            ep.ReqCode = req;
            ep.Value = value;
            ep.Index = index;
            return ep.Write(ref buf, ref len) && len == data.Length;
        }

        private bool CtrlIn(byte req, ushort value, ushort index, byte[] buf)
        {
            CyControlEndPoint ep = _ep;
            if (ep == null || buf == null)
            {
                return false;
            }
            int len = buf.Length;
            ep.Direction = 1;               // 1 = IN
            ep.ReqCode = req;
            ep.Value = value;
            ep.Index = index;
            return ep.Read(ref buf, ref len) && len == buf.Length;
        }

        // ------------------------------------------------------------------
        //  配置（SigmaStudio 会调用）
        // ------------------------------------------------------------------
        public void InitDataBytes(bool enable)
        {
            // 原版：true -> 64（FX2 固定打包），false -> 4096。
            // 我们对 ESP32 统一用 4096，减少 USB 往返次数。
            _maxDataBytes = MAX_XFER;
        }

        public void ChangeMaxDataBytes(int maxDataBytes)
        {
            // 夹在 [64, 4096]；更小没有意义，更大会超过固件缓冲
            if (maxDataBytes < 64) maxDataBytes = 64;
            if (maxDataBytes > MAX_XFER) maxDataBytes = MAX_XFER;
            _maxDataBytes = maxDataBytes;
        }

        public void SetSplitPackDelay(int splitDelay)
        {
            _splitDelayMs = splitDelay;
        }

        // ------------------------------------------------------------------
        //  控制类命令
        // ------------------------------------------------------------------
        public bool Reset()
        {
            lock (_lock)
            {
                try
                {
                    if (_ep != null) { _ep.Abort(); _ep.Reset(); }
                    if (_dev != null) { _dev.Reset(); }
                }
                catch (Exception)
                {
                    return false;
                }
            }
            return true;
        }

        public bool ResetDevice()
        {
            lock (_lock)
            {
                if (_ep == null) return false;
                return CtrlOut(CMD_RESET, 0, 0, new byte[0]);
            }
        }

        public bool ResetDeviceToggle(bool bEnabled)
        {
            lock (_lock)
            {
                if (_ep == null) return false;
                return CtrlOut(CMD_RESET_TOGGLE, (ushort)(bEnabled ? 1 : 0), 0, new byte[0]);
            }
        }

        public bool PowerBoard(bool bEnablePower)
        {
            lock (_lock)
            {
                if (_ep == null) return false;
                return CtrlOut(CMD_POWERBOARD, (ushort)(bEnablePower ? 1 : 0), 0, new byte[0]);
            }
        }

        public bool EnableLED(bool bEnable)
        {
            lock (_lock)
            {
                if (_ep == null) return false;
                return CtrlOut(CMD_LED, (ushort)(bEnable ? 1 : 0), 0, new byte[0]);
            }
        }

        // ------------------------------------------------------------------
        //  读
        // ------------------------------------------------------------------
        public bool ReadData(ref byte[] buf, int len, ref byte[] byteAddress,
                             int regByteLen, CommsFormat format, int chipAddress, int clatch)
        {
            if (buf == null || byteAddress == null || len <= 0 || len > buf.Length ||
                regByteLen <= 0 || byteAddress.Length < 2)
            {
                return false;
            }

            lock (_lock)
            {
                if (_ep == null) return false;

                int addr = (byteAddress[0] << 8) | byteAddress[1];
                int chunkMax = (_maxDataBytes / regByteLen) * regByteLen;
                if (chunkMax > MAX_XFER) chunkMax = MAX_XFER;
                if (chunkMax <= 0) chunkMax = regByteLen;

                int done = 0;
                while (done < len)
                {
                    int n = len - done;
                    if (n > chunkMax) n = chunkMax;

                    ushort value = (ushort)(chipAddress >> 1);
                    ushort index = (ushort)((clatch << 8) | ((int)format & 0xFF));

                    byte[] a = new byte[2];
                    a[0] = (byte)((addr >> 8) & 0xFF);
                    a[1] = (byte)(addr & 0xFF);
                    // 只发"设置读地址"和"读数据"两条；固件是同步完成的，
                    // 不需要再轮询 0xB5/0xB6（省 2 次 ~4ms 的控制传输）
                    if (!CtrlOut(CMD_WRITE_READ, value, index, a)) return false;
                    byte[] tmp = new byte[n];
                    if (!CtrlIn(CMD_READ, value, index, tmp)) return false;

                    Array.Copy(tmp, 0, buf, done, n);
                    done += n;
                    addr += n / regByteLen;
                }

                // 原版行为：把最终地址写回调用者的 byteAddress（原地修改）
                byteAddress[0] = (byte)((addr >> 8) & 0xFF);
                byteAddress[1] = (byte)(addr & 0xFF);
            }
            return true;
        }

        // ------------------------------------------------------------------
        //  写
        // ------------------------------------------------------------------
        public bool WriteData(ref byte[] buf, int len, int regByteLen,
                              CommsFormat format, int chipAddress, int clatch)
        {
            if (buf == null || len < 2 || len > buf.Length || regByteLen <= 0)
            {
                return false;
            }

            lock (_lock)
            {
                if (_ep == null) return false;

                int addr = (buf[0] << 8) | buf[1];
                int payloadLen = len - 2;
                int payloadMax = ((_maxDataBytes - 2) / regByteLen) * regByteLen;
                if (payloadMax > MAX_XFER - 2) payloadMax = MAX_XFER - 2;
                if (payloadMax <= 0) payloadMax = regByteLen;

                int done = 0;
                while (done < payloadLen)
                {
                    int n = payloadLen - done;
                    if (n > payloadMax) n = payloadMax;

                    byte[] d = new byte[n + 2];
                    d[0] = (byte)((addr >> 8) & 0xFF);
                    d[1] = (byte)(addr & 0xFF);
                    Array.Copy(buf, 2 + done, d, 2, n);

                    ushort value = (ushort)(chipAddress >> 1);
                    ushort index = (ushort)((clatch << 8) | ((int)format & 0xFF));
                    // 只发 0xB2；固件写完才结束控制传输，不必再轮询 0xB5
                    if (!CtrlOut(CMD_WRITE, value, index, d)) return false;

                    done += n;
                    addr += n / regByteLen;
                }
            }
            return true;
        }

        public bool WriteByteAddressData(ref byte[] buf, int len, int regByteLen,
                                         CommsFormat format, int chipAddress, int clatch)
        {
            return WriteData(ref buf, len, regByteLen, format, chipAddress, clatch);
        }

        // ------------------------------------------------------------------
        //  状态（SigmaStudio 在少数路径里会直接调用；正常读写不会再走这里）
        // ------------------------------------------------------------------
        public TransferStatus GetReadStatus(int chipAddress)
        {
            lock (_lock)
            {
                if (_ep == null) return TransferStatus.Exception;
                byte[] b = new byte[1];
                if (!CtrlIn(CMD_CHECK_READ, (ushort)(chipAddress >> 1), 0, b))
                {
                    return TransferStatus.Exception;
                }
                return (TransferStatus)b[0];
            }
        }

        public TransferStatus GetWriteStatus(int chipAddress)
        {
            lock (_lock)
            {
                if (_ep == null) return TransferStatus.Exception;
                byte[] b = new byte[1];
                if (!CtrlIn(CMD_CHECK_WRITE, (ushort)(chipAddress >> 1), 0, b))
                {
                    return TransferStatus.Exception;
                }
                return (TransferStatus)b[0];
            }
        }

        private bool waitForWrite(int chipAddress)
        {
            for (int i = 0; i < 100; i++)
            {
                TransferStatus st = GetWriteStatus(chipAddress);
                if (st == TransferStatus.Success) return true;
                if (st != TransferStatus.Writing) return false;
                Thread.Sleep(1);
            }
            return false;
        }

        private bool waitForRead(int chipAddress)
        {
            for (int i = 0; i < 100; i++)
            {
                TransferStatus st = GetReadStatus(chipAddress);
                if (st == TransferStatus.Success) return true;
                if (st != TransferStatus.Writing) return false;
                Thread.Sleep(1);
            }
            return false;
        }

        // ------------------------------------------------------------------
        //  清理
        // ------------------------------------------------------------------
        public void Dispose()
        {
            lock (_lock)
            {
                _ep = null;
                _dev = null;
                if (_list != null)
                {
                    try { _list.Dispose(); } catch (Exception) { }
                    _list = null;
                }
            }
        }
    }
}
