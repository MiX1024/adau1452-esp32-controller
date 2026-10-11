using System;
using System.IO;
using System.Web.Script.Serialization;
using Microsoft.Win32;

namespace DspVolumeCtl
{
    /// <summary>程序配置：ESP32 地址、端口、滚轮步进。保存为 exe 旁的 config.json（不可写时退回 %AppData%）。</summary>
    internal class AppConfig
    {
        // 用属性（而非字段）以便 JavaScriptSerializer 序列化/反序列化
        public string Ip { get; set; }
        public int Port { get; set; }
        public double WheelStep { get; set; }

        public AppConfig()
        {
            Ip = "";
            Port = 80;
            WheelStep = 2.0;
        }

        [ScriptIgnore]
        public string BaseUrl
        {
            get
            {
                string ip = Ip == null ? "" : Ip.Trim();
                return "http://" + ip + ":" + Port.ToString();
            }
        }

        [ScriptIgnore]
        public bool HasAddress
        {
            get { return !string.IsNullOrEmpty(Ip) && Ip.Trim().Length > 0; }
        }

        // ---- 存取 ----

        private static string ExeDir
        {
            get { return AppDomain.CurrentDomain.BaseDirectory; }
        }

        private static string AppDataPath
        {
            get
            {
                string dir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                    "AdaU1452VolumeCtl");
                return Path.Combine(dir, "config.json");
            }
        }

        public static AppConfig Load()
        {
            string[] candidates = new string[]
            {
                Path.Combine(ExeDir, "config.json"),
                AppDataPath,
            };
            JavaScriptSerializer js = new JavaScriptSerializer();
            foreach (string path in candidates)
            {
                try
                {
                    if (!File.Exists(path)) continue;
                    string json = File.ReadAllText(path);
                    AppConfig cfg = js.Deserialize<AppConfig>(json);
                    if (cfg != null)
                    {
                        if (cfg.Port < 1 || cfg.Port > 65535) cfg.Port = 80;
                        if (cfg.WheelStep <= 0 || cfg.WheelStep > 20) cfg.WheelStep = 2.0;
                        return cfg;
                    }
                }
                catch { }
            }
            return new AppConfig();
        }

        public void Save()
        {
            JavaScriptSerializer js = new JavaScriptSerializer();
            string json = js.Serialize(this);
            try
            {
                File.WriteAllText(Path.Combine(ExeDir, "config.json"), json);
                return;
            }
            catch { }
            try
            {
                string path = AppDataPath;
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllText(path, json);
            }
            catch { }
        }
    }

    /// <summary>开机自启：HKCU Run 键（无需管理员权限，随当前用户登录启动）。</summary>
    internal static class Autostart
    {
        private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
        private const string ValueName = "AdaU1452VolumeCtl";

        public static bool IsEnabled()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKeyPath, false))
                {
                    if (key == null) return false;
                    return key.GetValue(ValueName) != null;
                }
            }
            catch { return false; }
        }

        public static void SetEnabled(bool enabled)
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RunKeyPath, true))
                {
                    if (key == null) return;
                    if (enabled)
                    {
                        string exe = System.Diagnostics.Process.GetCurrentProcess().MainModule.FileName;
                        key.SetValue(ValueName, "\"" + exe + "\"");
                    }
                    else
                    {
                        key.DeleteValue(ValueName, false);
                    }
                }
            }
            catch { }
        }
    }
}
