using System;
using System.Drawing;
using System.Net.Http;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace DspVolumeCtl
{
    /// <summary>设置对话框：ESP32 IP/端口、开机自启、连接测试。</summary>
    internal class SettingsForm : Form
    {
        private readonly TrayApp _owner;
        private readonly TextBox _ipBox;
        private readonly NumericUpDown _portBox;
        private readonly CheckBox _autostart;
        private readonly Label _status;
        private readonly Button _btnTest;
        private readonly Button _btnSave;
        private readonly Button _btnCancel;

        public string NewIp { get; private set; }
        public int NewPort { get; private set; }

        public SettingsForm(TrayApp owner)
        {
            _owner = owner;

            Text = "设置";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterScreen;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            BackColor = Theme.Back;
            ForeColor = Theme.Text;
            ClientSize = new Size(380, 250);
            Font = Theme.Font(9.5f, false);

            Label lblIp = MakeLabel("ESP32 控制器 IP 地址", 18, 18, 340, 22);
            _ipBox = new TextBox();
            _ipBox.Location = new Point(18, 44);
            _ipBox.Size = new Size(232, 26);
            _ipBox.Font = Theme.Font(11f, false);
            _ipBox.Text = owner.Config.Ip;
            Controls.Add(_ipBox);

            Label lblPort = MakeLabel("端口", 262, 18, 100, 22);
            _portBox = new NumericUpDown();
            _portBox.Location = new Point(262, 44);
            _portBox.Size = new Size(98, 26);
            _portBox.Minimum = 1;
            _portBox.Maximum = 65535;
            _portBox.Value = owner.Config.Port;
            Controls.Add(_portBox);

            _autostart = new CheckBox();
            _autostart.Text = "开机自动启动（当前用户登录时运行）";
            _autostart.Location = new Point(18, 88);
            _autostart.Size = new Size(340, 24);
            _autostart.Checked = Autostart.IsEnabled();
            _autostart.ForeColor = Theme.Text;
            Controls.Add(_autostart);

            _status = MakeLabel("提示：IP 可在设备 OLED 屏幕或路由器后台查看", 18, 122, 344, 44);
            _status.ForeColor = Theme.TextDim;

            _btnTest = MakeButton("测试连接", 18, 182, 100);
            _btnTest.Click += OnTestClick;

            _btnSave = MakeButton("保存", 158, 182, 100);
            _btnSave.Click += OnSaveClick;

            _btnCancel = MakeButton("取消", 262, 182, 100);
            _btnCancel.Click += delegate { DialogResult = DialogResult.Cancel; Close(); };

            AcceptButton = _btnSave;
            CancelButton = _btnCancel;
        }

        private Label MakeLabel(string text, int x, int y, int w, int h)
        {
            Label l = new Label();
            l.Text = text;
            l.Location = new Point(x, y);
            l.Size = new Size(w, h);
            l.ForeColor = Theme.Text;
            Controls.Add(l);
            return l;
        }

        private Button MakeButton(string text, int x, int y, int w)
        {
            Button b = new Button();
            b.Text = text;
            b.Location = new Point(x, y);
            b.Size = new Size(w, 32);
            b.FlatStyle = FlatStyle.Flat;
            b.FlatAppearance.BorderColor = Theme.Track;
            b.FlatAppearance.MouseOverBackColor = Theme.Panel;
            b.BackColor = Theme.Panel;
            b.ForeColor = Theme.Text;
            b.Cursor = Cursors.Hand;
            Controls.Add(b);
            return b;
        }

        private async void OnTestClick(object sender, EventArgs e)
        {
            _btnTest.Enabled = false;
            _status.ForeColor = Theme.TextDim;
            _status.Text = "正在连接…";
            try
            {
                AppConfig tmp = new AppConfig();
                tmp.Ip = _ipBox.Text.Trim();
                tmp.Port = (int)_portBox.Value;
                using (DspClient client = new DspClient())
                {
                    DeviceState st = await client.PollAsync(tmp);
                    string dsp = st.DspOnline ? "DSP 在线" : "DSP 离线";
                    string master = st.Ch[0].Name + " " + st.Ch[0].Value.ToString("0") + " " + st.Ch[0].Unit;
                    _status.ForeColor = Theme.Ok;
                    _status.Text = "连接成功：" + dsp + "，" + master + "，WiFi " +
                                   st.WifiRssi.ToString("0") + " dBm";
                }
            }
            catch (Exception ex)
            {
                _status.ForeColor = Theme.Err;
                _status.Text = "连接失败：" + ex.Message;
            }
            finally
            {
                _btnTest.Enabled = true;
            }
        }

        private void OnSaveClick(object sender, EventArgs e)
        {
            string ip = _ipBox.Text.Trim();
            if (ip.Length == 0)
            {
                MessageBox.Show(this, "请输入 ESP32 的 IP 地址（或主机名）", "设置",
                                MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            NewIp = ip;
            NewPort = (int)_portBox.Value;
            Autostart.SetEnabled(_autostart.Checked);
            DialogResult = DialogResult.OK;
            Close();
        }
    }
}
