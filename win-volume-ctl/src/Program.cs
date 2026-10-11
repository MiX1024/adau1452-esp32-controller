using System;
using System.Threading;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    internal static class Program
    {
        [STAThread]
        private static void Main()
        {
            bool createdNew;
            using (Mutex mutex = new Mutex(true, "Global\\AdaU1452VolumeCtl_SingleInstance", out createdNew))
            {
                if (!createdNew) return;   // 已有实例在跑

                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new TrayApp());
                GC.KeepAlive(mutex);
            }
        }
    }
}
