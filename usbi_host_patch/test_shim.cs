using System;
using System.Diagnostics;
using Analog.ADIUSBi;

class TestShim
{
    static Stopwatch sw = new Stopwatch();

    static void Main()
    {
        ADIUSBi d = ADIUSBi.USBi;
        d.InitDataBytes(true);
        Console.WriteLine("connected = " + d.IsDeivceConnected);
        if (!d.IsDeivceConnected) { Console.WriteLine("DEVICE NOT FOUND"); return; }

        // 4B 参数读
        byte[] addr = new byte[] { 0x00, 0x7A };
        byte[] buf = new byte[4];
        sw.Restart();
        bool ok = d.ReadData(ref buf, 4, ref addr, 4, CommsFormat.I2C, 0x76, 0);
        sw.Stop();
        Console.WriteLine("ReadData 4B: ok=" + ok + " data=" + BitConverter.ToString(buf) + " addr=" + BitConverter.ToString(addr) + " " + sw.Elapsed.TotalMilliseconds.ToString("F2") + "ms");

        // 原值写回
        byte[] w = new byte[6];
        w[0] = 0x00; w[1] = 0x7A;
        Array.Copy(buf, 0, w, 2, 4);
        sw.Restart();
        ok = d.WriteData(ref w, 6, 4, CommsFormat.I2C, 0x76, 0);
        sw.Stop();
        Console.WriteLine("WriteData 4B: ok=" + ok + " " + sw.Elapsed.TotalMilliseconds.ToString("F2") + "ms");

        byte[] a2 = new byte[] { 0x00, 0x7A };
        byte[] buf2 = new byte[4];
        ok = d.ReadData(ref buf2, 4, ref a2, 4, CommsFormat.I2C, 0x76, 0);
        Console.WriteLine("Readback 4B: ok=" + ok + " data=" + BitConverter.ToString(buf2) + " match=" + (BitConverter.ToString(buf) == BitConverter.ToString(buf2)));

        // 4KB 程序区读写（写入相同数据）
        byte[] bigAddr = new byte[] { 0xC0, 0x00 };
        byte[] big = new byte[4092];
        sw.Restart();
        ok = d.ReadData(ref big, 4092, ref bigAddr, 4, CommsFormat.I2C, 0x76, 0);
        sw.Stop();
        Console.WriteLine("ReadData 4092B: ok=" + ok + " " + sw.Elapsed.TotalMilliseconds.ToString("F2") + "ms");

        byte[] bigW = new byte[4092 + 2];
        bigW[0] = 0xC0; bigW[1] = 0x00;
        Array.Copy(big, 0, bigW, 2, 4092);
        sw.Restart();
        ok = d.WriteData(ref bigW, bigW.Length, 4, CommsFormat.I2C, 0x76, 0);
        sw.Stop();
        Console.WriteLine("WriteData 4092B: ok=" + ok + " " + sw.Elapsed.TotalMilliseconds.ToString("F2") + "ms");

        byte[] bigAddr2 = new byte[] { 0xC0, 0x00 };
        byte[] back = new byte[4092];
        ok = d.ReadData(ref back, 4092, ref bigAddr2, 4, CommsFormat.I2C, 0x76, 0);
        int bad = 0;
        for (int i = 0; i < 4092; i++) if (back[i] != big[i]) bad++;
        Console.WriteLine("4KB readback: " + (bad == 0 ? "MATCH" : ("MISMATCH " + bad)));

        // 状态命令
        Console.WriteLine("GetWriteStatus = " + d.GetWriteStatus(0x76));
        Console.WriteLine("GetReadStatus  = " + d.GetReadStatus(0x76));
        Console.WriteLine("PowerBoard = " + d.PowerBoard(true) + ", LED = " + d.EnableLED(true));
    }
}
