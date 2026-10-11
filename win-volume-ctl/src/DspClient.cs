using System;
using System.Collections;
using System.Collections.Generic;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace DspVolumeCtl
{
    /// <summary>单个音量通道的状态（单位与设备端一致：MASTER 用 %，其余用 dB）。</summary>
    internal class ChannelState
    {
        public string Name = "";
        public string Unit = "";
        public double Value;
        public double Min;
        public double Max;
    }

    /// <summary>一次 /api/vol 轮询的完整状态。</summary>
    internal class DeviceState
    {
        public bool DspOnline;
        public bool Safe;
        public ChannelState[] Ch = new ChannelState[4];
        public double LevelL = -999.0;
        public double LevelR = -999.0;
        public bool BridgeEnabled;
        public bool BridgeLocal;
        public bool BridgeRunning;
        public bool BridgeClient;
        public bool WifiConnected;
        public double WifiRssi;
        public double WifiCh;
        public string WifiIp = "";
        public string WifiBssid = "";
        public string ApSsid = "";
        public DateTime Stamp = DateTime.MinValue;

        public DeviceState()
        {
            for (int i = 0; i < 4; i++)
            {
                Ch[i] = new ChannelState();
                Ch[i].Name = i == 0 ? "MASTER" : (i == 1 ? "LOW" : (i == 2 ? "HIGH" : "MID"));
                Ch[i].Unit = i == 0 ? "%" : "dB";
                Ch[i].Min = i == 0 ? 0 : -40;
                Ch[i].Max = i == 0 ? 120 : 20;
            }
        }
    }

    /// <summary>设置音量的结果。</summary>
    internal class SetResult
    {
        public bool Ok;
        public string Reason = "";
        public double Value;
    }

    /// <summary>
    /// ESP32 通信层。全部走 HTTP：
    ///   GET  /api/vol        轮询状态（音量/电平/桥接/安全模式/WiFi）
    ///   POST /api/vol        设置音量 {"idx":N,"value":v}
    ///   POST /api/bridge     桥接开关 {"enabled":bool} / 本地控制 {"local_control":bool}
    ///   POST /api/safe       安全模式 {"on":bool}
    /// 不碰 WebSocket，与网页控制面板零冲突。
    /// </summary>
    internal class DspClient : IDisposable
    {
        private readonly HttpClient _http;
        private readonly JavaScriptSerializer _js = new JavaScriptSerializer();

        public DspClient()
        {
            _http = new HttpClient();
            _http.Timeout = TimeSpan.FromMilliseconds(2000);
        }

        // ---- JSON 小工具 ----

        private static Dictionary<string, object> AsObj(object o)
        {
            return o as Dictionary<string, object>;
        }

        private static double Num(object o, double def)
        {
            if (o is int) return (int)o;
            if (o is long) return (long)o;
            if (o is decimal) return (double)(decimal)o;
            if (o is double) return (double)o;
            if (o is float) return (double)(float)o;
            return def;
        }

        private static bool Bool(object o, bool def)
        {
            return o is bool ? (bool)o : def;
        }

        private static string Str(object o, string def)
        {
            return o is string ? (string)o : def;
        }

        private static IEnumerable AsList(object o)
        {
            return o as IEnumerable;
        }

        private string Post(string url, Dictionary<string, object> body)
        {
            string json = _js.Serialize(body);
            /*
             * 失败重试一次：HttpClient 复用 keep-alive 连接时偶尔会撞上
             * ESP32 已关闭的 socket（"连接被强制关闭"）—— 重试一次即好。
             * 音量设置是幂等的，重发安全。
             */
            Exception last = null;
            for (int attempt = 0; attempt < 2; attempt++)
            {
                try
                {
                    using (HttpContent content = new StringContent(json, Encoding.UTF8, "application/json"))
                    {
                        HttpResponseMessage resp = _http.PostAsync(url, content).Result;
                        string text = resp.Content.ReadAsStringAsync().Result;
                        if (!resp.IsSuccessStatusCode)
                        {
                            throw new HttpRequestException("HTTP " + ((int)resp.StatusCode).ToString() + " " + text);
                        }
                        return text;
                    }
                }
                catch (AggregateException e)
                {
                    last = e.InnerException != null ? e.InnerException : e;
                }
                catch (Exception e)
                {
                    last = e;
                }
            }
            throw last;
        }

        // ---- 接口 ----

        public async Task<DeviceState> PollAsync(AppConfig cfg)
        {
            string url = cfg.BaseUrl + "/api/vol";
            HttpResponseMessage resp = await _http.GetAsync(url);
            resp.EnsureSuccessStatusCode();
            string text = await resp.Content.ReadAsStringAsync();
            return ParseState(text);
        }

        private DeviceState ParseState(string text)
        {
            DeviceState st = new DeviceState();
            Dictionary<string, object> root = AsObj(_js.DeserializeObject(text));
            if (root == null) return st;

            st.DspOnline = Bool(root["dsp"], false);
            st.Safe = Bool(root["safe"], false);

            IEnumerable chArr = AsList(root["ch"]);
            if (chArr != null)
            {
                int i = 0;
                foreach (object item in chArr)
                {
                    if (i >= 4) break;
                    Dictionary<string, object> o = AsObj(item);
                    if (o != null)
                    {
                        st.Ch[i].Name = Str(o["name"], st.Ch[i].Name);
                        st.Ch[i].Unit = Str(o["unit"], st.Ch[i].Unit);
                        st.Ch[i].Value = Num(o["value"], st.Ch[i].Value);
                        st.Ch[i].Min = Num(o["min"], st.Ch[i].Min);
                        st.Ch[i].Max = Num(o["max"], st.Ch[i].Max);
                    }
                    i++;
                }
            }

            Dictionary<string, object> lv = AsObj(root["level"]);
            if (lv != null)
            {
                st.LevelL = Num(lv["l"], -999.0);
                st.LevelR = Num(lv["r"], -999.0);
            }

            Dictionary<string, object> br = AsObj(root["bridge"]);
            if (br != null)
            {
                st.BridgeEnabled = Bool(br["enabled"], false);
                st.BridgeLocal = Bool(br["local_control"], true);
                st.BridgeRunning = Bool(br["running"], false);
                st.BridgeClient = Bool(br["client"], false);
            }

            Dictionary<string, object> wf = AsObj(root["wifi"]);
            if (wf != null)
            {
                st.WifiConnected = Bool(wf["connected"], false);
                st.WifiRssi = Num(wf["rssi"], -127);
                st.WifiCh = Num(wf["ch"], 0);
                st.WifiIp = Str(wf["ip"], "");
                st.WifiBssid = Str(wf["bssid"], "");
                st.ApSsid = Str(wf["ap_ssid"], "");
            }

            st.Stamp = DateTime.Now;
            return st;
        }

        public async Task<SetResult> SetVolumeAsync(AppConfig cfg, int idx, double value)
        {
            Dictionary<string, object> body = new Dictionary<string, object>();
            body["idx"] = idx;
            body["value"] = value;
            string text = await Task.Run(() => Post(cfg.BaseUrl + "/api/vol", body));
            SetResult r = new SetResult();
            Dictionary<string, object> root = AsObj(_js.DeserializeObject(text));
            if (root != null)
            {
                r.Ok = Bool(root["ok"], false);
                r.Reason = Str(root["reason"], "");
                r.Value = Num(root["value"], value);
            }
            return r;
        }

        public async Task<bool> SetBridgeAsync(AppConfig cfg, bool enabled)
        {
            Dictionary<string, object> body = new Dictionary<string, object>();
            body["enabled"] = enabled;
            string text = await Task.Run(() => Post(cfg.BaseUrl + "/api/bridge", body));
            Dictionary<string, object> root = AsObj(_js.DeserializeObject(text));
            return root != null && Bool(root["ok"], false);
        }

        public async Task<bool> SetLocalControlAsync(AppConfig cfg, bool on)
        {
            Dictionary<string, object> body = new Dictionary<string, object>();
            body["local_control"] = on;
            string text = await Task.Run(() => Post(cfg.BaseUrl + "/api/bridge", body));
            Dictionary<string, object> root = AsObj(_js.DeserializeObject(text));
            return root != null && Bool(root["ok"], false);
        }

        public async Task<bool> SetSafeAsync(AppConfig cfg, bool on)
        {
            Dictionary<string, object> body = new Dictionary<string, object>();
            body["on"] = on;
            string text = await Task.Run(() => Post(cfg.BaseUrl + "/api/safe", body));
            Dictionary<string, object> root = AsObj(_js.DeserializeObject(text));
            return root != null && Bool(root["ok"], false);
        }

        public void Dispose()
        {
            _http.Dispose();
        }
    }
}
