using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading.Tasks;
using RuntimeSwitcher.Core;
using Xunit;

namespace RuntimeSwitcher.Tests
{
    public class RuntimeListTests
    {
        private const string Ours = @"C:\Users\u\AppData\Local\Programs\OpenLamina";
        private const string Mirador = @"C:\Users\u\AppData\Local\Mirador\OpenComposite";
        private const string SteamVR = @"C:\Program Files (x86)\Steam\steamapps\common\SteamVR";
        private const string Other = @"D:\Runtimes\Something";

        [Fact]
        public void UseOpenCompositePutsOursFirstAndKeepsTheRest()
        {
            var result = RuntimeList.UseOpenComposite(new[] { SteamVR, Mirador, Other }, Ours, true);
            Assert.Equal(new[] { Ours, SteamVR, Mirador, Other }, result);
        }

        [Fact]
        public void UseOpenCompositeDoesNotDuplicateOursWhateverItsCaseOrTrailingSlash()
        {
            var result = RuntimeList.UseOpenComposite(new[] { SteamVR, Ours.ToUpperInvariant() + "\\" }, Ours, true);
            Assert.Equal(new[] { Ours, SteamVR }, result);
        }

        [Theory]
        [InlineData(@"C:\Users\u\AppData\Local\Programs\OpenLamina", true)]
        [InlineData("C:/Users/u/AppData/Local/Programs/OpenLamina/", true)]
        [InlineData(@"C:\Users\u/AppData/Local\Programs\OpenLamina\\", true)]
        [InlineData(@"C:\Users\u\AppData\Local\Programs\OpenLamina2", false)]
        public void SamePathFoldsSeparators(string path, bool same)
        {
            Assert.Equal(same, RuntimeList.SamePath(path, Ours, true));
        }

        [Fact]
        public void UseOpenCompositeOnAnEmptyList()
        {
            Assert.Equal(new[] { Ours }, RuntimeList.UseOpenComposite(Array.Empty<string>(), Ours, true));
        }

        [Fact]
        public void UseSteamVRRemovesOursAndMovesSteamVRFirst()
        {
            var result = RuntimeList.UseSteamVR(new[] { Ours, Mirador, SteamVR, Other }, Ours, true);
            Assert.Equal(new[] { SteamVR, Mirador, Other }, result);
        }

        [Fact]
        public void UseSteamVRLeavesMiradorsCopyInTheList()
        {
            var result = RuntimeList.UseSteamVR(new[] { Mirador, SteamVR }, Ours, true);
            Assert.Equal(new[] { SteamVR, Mirador }, result);
        }

        [Fact]
        public void UseSteamVRWithoutASteamVREntryOnlyRemovesOurs()
        {
            var result = RuntimeList.UseSteamVR(new[] { Ours, Other }, Ours, true);
            Assert.Equal(new[] { Other }, result);
        }

        [Fact]
        public void RemoveWhenOursIsFirstPutsSteamVRFirst()
        {
            var result = RuntimeList.Remove(new[] { Ours, Other, SteamVR }, Ours, true);
            Assert.Equal(new[] { SteamVR, Other }, result);
        }

        [Fact]
        public void RemoveWhenOursIsNotFirstKeepsTheOrder()
        {
            var result = RuntimeList.Remove(new[] { Mirador, Ours, SteamVR }, Ours, true);
            Assert.Equal(new[] { Mirador, SteamVR }, result);
        }

        [Fact]
        public void CaseMattersWhenAskedTo()
        {
            var result = RuntimeList.UseOpenComposite(new[] { "/home/u/OpenLamina" }, "/home/u/opencomposite", false);
            Assert.Equal(new[] { "/home/u/opencomposite", "/home/u/OpenLamina" }, result);
        }

        [Theory]
        [InlineData(Ours, RuntimeKind.OpenComposite)]
        [InlineData(Mirador, RuntimeKind.Mirador)]
        [InlineData(SteamVR, RuntimeKind.SteamVR)]
        [InlineData(@"C:\Steam\steamapps\common\SteamVR\", RuntimeKind.SteamVR)]
        [InlineData("/home/u/.local/share/Steam/steamapps/common/SteamVR", RuntimeKind.SteamVR)]
        [InlineData(@"C:\SteamVR Tools\Other", RuntimeKind.Other)]
        [InlineData(null, RuntimeKind.None)]
        public void Classify(string path, RuntimeKind kind)
        {
            Assert.Equal(kind, RuntimeList.Classify(path, Ours, Mirador, true));
        }
    }

    public class OpenVRPathsTests : IDisposable
    {
        private readonly string _folder = Path.Combine(Path.GetTempPath(), "oc-tests-" + Guid.NewGuid().ToString("N"));

        public void Dispose()
        {
            if (Directory.Exists(_folder))
                Directory.Delete(_folder, true);
        }

        private const string SteamsFile = "{\n" +
                                          "\t\"config\" : \n\t[\n\t\t\"C:\\\\Program Files (x86)\\\\Steam\\\\config\"\n\t],\n" +
                                          "\t\"external_drivers\" : \n\t[\n\t\t\"C:\\\\Drivers\\\\example\"\n\t],\n" +
                                          "\t\"jsonid\" : \"vrpathreg\",\n" +
                                          "\t\"log\" : \n\t[\n\t\t\"C:\\\\Program Files (x86)\\\\Steam\\\\logs\"\n\t],\n" +
                                          "\t\"runtime\" : \n\t[\n\t\t\"C:\\\\Program Files (x86)\\\\Steam\\\\steamapps\\\\common\\\\SteamVR\"\n\t],\n" +
                                          "\t\"unknown\" : { \"when\": \"2026-11-04T10:00:00Z\", \"ratio\": 1.50 },\n" +
                                          "\t\"version\" : 1\n}\n";

        [Fact]
        public void KeepsEveryOtherKeyAndTheirOrder()
        {
            string file = Path.Combine(_folder, "openvrpaths.vrpath");
            Directory.CreateDirectory(_folder);
            File.WriteAllText(file, SteamsFile);

            OpenVRPaths paths = OpenVRPaths.Load(file);
            Assert.True(paths.Existed);
            paths.SetRuntimes(RuntimeList.UseOpenComposite(paths.Runtimes, @"C:\OC", true));
            paths.Save();

            var json = Newtonsoft.Json.Linq.JObject.Parse(File.ReadAllText(file),
                new Newtonsoft.Json.Linq.JsonLoadSettings());
            Assert.Equal(new[] { "config", "external_drivers", "jsonid", "log", "runtime", "unknown", "version" },
                json.Properties().Select(p => p.Name));
            Assert.Equal(new[] { @"C:\OC", @"C:\Program Files (x86)\Steam\steamapps\common\SteamVR" },
                json["runtime"].Select(t => (string)t));
            Assert.Equal(@"C:\Drivers\example", (string)json["external_drivers"][0]);
            string text = File.ReadAllText(file);
            Assert.Contains("\"2026-11-04T10:00:00Z\"", text);
            Assert.Contains("1.50", text);
            Assert.Contains("\n\t\"jsonid\"", text);
            Assert.DoesNotContain("\r", text);
            Assert.Empty(Directory.GetFiles(_folder, "*.tmp"));
            Assert.Equal(SteamsFile, File.ReadAllText(file + ".bak"));
        }

        [Fact]
        public void SwitchingThereAndBackGivesTheSameRuntimes()
        {
            string file = Path.Combine(_folder, "openvrpaths.vrpath");
            Directory.CreateDirectory(_folder);
            File.WriteAllText(file, SteamsFile);
            IReadOnlyList<string> before = OpenVRPaths.Load(file).Runtimes;

            OpenVRPaths paths = OpenVRPaths.Load(file);
            paths.SetRuntimes(RuntimeList.UseOpenComposite(paths.Runtimes, @"C:\OC", true));
            paths.Save();
            paths = OpenVRPaths.Load(file);
            paths.SetRuntimes(RuntimeList.UseSteamVR(paths.Runtimes, @"C:\OC", true));
            paths.Save();

            Assert.Equal(before, OpenVRPaths.Load(file).Runtimes);
        }

        [Fact]
        public void AMissingFileIsCreatedWithItsFolder()
        {
            string file = Path.Combine(_folder, "openvr", "openvrpaths.vrpath");
            OpenVRPaths paths = OpenVRPaths.Load(file);
            Assert.False(paths.Existed);
            Assert.Empty(paths.Runtimes);
            paths.SetRuntimes(new[] { @"C:\OC" });
            paths.Save();

            var json = Newtonsoft.Json.Linq.JObject.Parse(File.ReadAllText(file));
            Assert.Equal("vrpathreg", (string)json["jsonid"]);
            Assert.Equal(1, (int)json["version"]);
            Assert.Equal(@"C:\OC", (string)json["runtime"][0]);
        }

        [Theory]
        [InlineData("not json")]
        [InlineData("{\"runtime\": \"C:\\\\one\"}")]
        [InlineData("{\"runtime\": [1, 2]}")]
        [InlineData("{\"runtime\": []} trailing")]
        [InlineData("[]")]
        public void ABrokenFileIsRefused(string text)
        {
            Assert.Throws<InvalidDataException>(() => OpenVRPaths.Parse("openvrpaths.vrpath", text));
        }

        [Fact]
        public void NoRuntimeKeyIsAnEmptyList()
        {
            Assert.Empty(OpenVRPaths.Parse("f", "{\"jsonid\": \"vrpathreg\"}").Runtimes);
        }

        [Fact]
        public void AReadOnlyFileIsNotChanged()
        {
            if (!OperatingSystem.IsWindows())
                return; // The read-only attribute is Windows'.
            string file = Path.Combine(_folder, "openvrpaths.vrpath");
            Directory.CreateDirectory(_folder);
            File.WriteAllText(file, SteamsFile);
            File.SetAttributes(file, FileAttributes.ReadOnly);
            try
            {
                OpenVRPaths paths = OpenVRPaths.Load(file);
                paths.SetRuntimes(new[] { @"C:\OC" });
                Assert.Throws<IOException>(paths.Save);
                Assert.Equal(SteamsFile, File.ReadAllText(file));
            }
            finally
            {
                File.SetAttributes(file, FileAttributes.Normal);
            }
        }
    }

    public class ReleaseTests
    {
        private const string Sha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

        private static string Json(params (string tag, bool pre, bool draft, bool asset, bool digest)[] releases) =>
            "[" + string.Join(",", releases.Select(r =>
                $"{{\"tag_name\":\"{r.tag}\",\"prerelease\":{(r.pre ? "true" : "false")},\"draft\":{(r.draft ? "true" : "false")}," +
                $"\"html_url\":\"https://github.com/x/y/releases/tag/{r.tag}\",\"assets\":[" +
                (r.asset
                    ? $"{{\"name\":\"OpenLamina-Windows-Installer.exe\",\"browser_download_url\":\"https://example/{r.tag}/OpenLamina-Windows-Installer.exe\",\"size\":10" +
                      (r.digest ? $",\"digest\":\"sha256:{Sha.ToUpperInvariant()}\"" : ",\"digest\":null") + "}"
                    : "") +
                "]}")) + "]";

        private static Release Find(string json, string installed, bool pre) =>
            Releases.FindUpdate(Releases.Parse(json), installed, pre, "OpenLamina-Windows-Installer.exe");

        [Fact]
        public void TheNewestStableReleaseWithTheSetup()
        {
            string json = Json(("v2026.12.01", false, false, true, true), ("v2026.12.10", true, false, true, true),
                ("v2026.11.20", false, false, true, true));
            Assert.Equal("2026.12.01", Find(json, "2026.11.06", false)?.Version);
            Assert.Equal("2026.12.10", Find(json, "2026.11.06", true)?.Version);
        }

        [Fact]
        public void NothingWhenUpToDate()
        {
            string json = Json(("v2026.11.06", false, false, true, true));
            Assert.Null(Find(json, "2026.11.06", true));
            Assert.Null(Find(json, "2026.11.07", true));
        }

        [Fact]
        public void ABuildOfNoReleaseIsOlderThanAll()
        {
            Assert.Equal("2026.11.06", Find(Json(("v2026.11.06", false, false, true, true)), null, false)?.Version);
        }

        [Fact]
        public void DraftsAndReleasesWithoutTheSetupOrItsDigestAreSkipped()
        {
            string json = Json(("v2026.12.03", false, true, true, true), ("v2026.12.02", false, false, false, false),
                ("v2026.12.01", false, false, true, false), ("v2026.11.20", false, false, true, true));
            Release found = Find(json, "2026.11.06", true);
            Assert.Equal("2026.11.20", found?.Version);
            Assert.Equal(Sha, found?.Asset("OpenLamina-Windows-Installer.exe")?.Sha256);
        }

        [Fact]
        public void TheOnlyPreReleaseIsFoundOnlyWhenAskedFor()
        {
            // A repository whose only release is a pre-release.
            string json = Json(("v2026.11.06", true, false, true, true));
            Assert.Null(Find(json, null, false));
            Assert.NotNull(Find(json, null, true));
        }

        [Theory]
        [InlineData("[1]")]
        [InlineData("[{\"tag_name\":\"v1\",\"prerelease\":\"maybe\"}]")]
        [InlineData("[{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"a\",\"size\":\"big\"}]}]")]
        [InlineData("[{\"tag_name\":\"v1\",\"assets\":[\"a\"]}]")]
        public void ReleasesOfAnotherShapeAreRefused(string json)
        {
            Assert.Throws<InvalidDataException>(() => Releases.Parse(json));
        }

        [Theory]
        [InlineData("2026.11.06", "2026.11.6", 0)]
        [InlineData("2026.11.10", "2026.11.9", 1)]
        [InlineData("2026.12.01", "2026.11.30", 1)]
        [InlineData("2026.11.06.1", "2026.11.06", 1)]
        [InlineData("0.0.0-test.1", "0.0.0", -1)]
        [InlineData("0.0.0-test.2", "0.0.0-test.1", 1)]
        [InlineData("1.0.0-rc.10", "1.0.0-rc.2", 1)]
        [InlineData("1.0.0-rc.1", "1.0.0-rc", 1)]
        [InlineData("1.0.0-beta", "1.0.0-2", 1)]
        [InlineData("1.0.0-beta.2", "1.0.0-alpha.10", 1)]
        [InlineData("1.0.0-rc.1", "1.0.0-rc.1", 0)]
        [InlineData("1.0.0-rc.a-1", "1.0.0-rc.1", 1)]
        [InlineData("not-a-version", "2026.11.06", -1)]
        public void VersionsCompareAsNumbers(string a, string b, int sign)
        {
            Assert.Equal(sign, Math.Sign(ReleaseVersion.Compare(a, b)));
            Assert.Equal(-sign, Math.Sign(ReleaseVersion.Compare(b, a)));
        }

        [Theory]
        [InlineData("2026.11.06", "2026.11.6.0")]
        [InlineData("1.2", "1.2.0.0")]
        [InlineData("0.0.0-test.1", "0.0.0.0")]
        [InlineData("dev-0123456789ab", null)]
        [InlineData("70000.1.1", null)]
        public void NumericVersions(string version, string numeric)
        {
            Assert.Equal(numeric, ReleaseVersion.Numeric(version));
        }

        [Theory]
        [InlineData("OpenLamina 2026.11.06, commit 7534249abc\nFrom https://…\n", "2026.11.06")]
        [InlineData("OpenComposite 2026.11.06, commit 7534249abc\nFrom https://…\n", "2026.11.06")] // written before OpenLamina
        [InlineData("OpenLamina dev-0123456789ab, commit 0123456789abcdef\n", null)]
        [InlineData("something else\n", null)]
        [InlineData("", null)]
        public void TheInstalledVersionFromSourceTxt(string text, string version)
        {
            Assert.Equal(version, InstalledVersion.FromSourceText(text));
        }

        [Fact]
        public void OldDownloadsAreDeletedButNotTheCurrentOne()
        {
            string folder = Path.Combine(Path.GetTempPath(), "oc-tests-" + Guid.NewGuid().ToString("N"));
            try
            {
                Directory.CreateDirectory(Path.Combine(folder, "2026.11.06"));
                File.WriteAllText(Path.Combine(folder, "2026.11.06", "OpenLamina-Windows-Installer.exe"), "old");
                Directory.CreateDirectory(Path.Combine(folder, "2026.12.01"));
                Updater.DeleteOtherFolders(folder, "2026.12.01");
                Assert.Equal(new[] { "2026.12.01" }, Directory.GetDirectories(folder).Select(Path.GetFileName));
                Updater.DeleteOtherFolders(Path.Combine(folder, "missing"), "2026.12.01");
            }
            finally
            {
                Directory.Delete(folder, true);
            }
        }

        [Fact]
        public void TheDownloadIsCheckedAgainstTheDigest()
        {
            string file = Path.GetTempFileName();
            try
            {
                File.WriteAllText(file, "abc");
                Assert.Equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Updater.Sha256Of(file));
            }
            finally
            {
                File.Delete(file);
            }
        }
    }

    public class DownloadTests : IDisposable
    {
        private readonly string _folder = Path.Combine(Path.GetTempPath(), "oc-tests-" + Guid.NewGuid().ToString("N"));
        private readonly TcpListener _listener = new TcpListener(IPAddress.Loopback, 0);

        public DownloadTests() => _listener.Start();

        public void Dispose()
        {
            _listener.Stop();
            if (Directory.Exists(_folder))
                Directory.Delete(_folder, true);
        }

        private static string Sha256Hex(byte[] data)
        {
            using (var sha = System.Security.Cryptography.SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(data)).Replace("-", "").ToLowerInvariant();
        }

        /// <summary>
        /// Answers one request with <paramref name="body"/> (a Content-Length of all of it); with <paramref name="stall"/>,
        /// sends only its first half and then nothing until the client goes or ten seconds pass.
        /// </summary>
        private async Task Serve(byte[] body, bool stall = false)
        {
            using (TcpClient client = await _listener.AcceptTcpClientAsync())
            using (NetworkStream stream = client.GetStream())
            {
                var request = new byte[65536];
                int got = 0;
                while (got < request.Length)
                {
                    int n = await stream.ReadAsync(request, got, request.Length - got);
                    if (n == 0)
                        return;
                    got += n;
                    if (Encoding.ASCII.GetString(request, 0, got).Contains("\r\n\r\n"))
                        break;
                }

                byte[] head = Encoding.ASCII.GetBytes(
                    $"HTTP/1.1 200 OK\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n");
                try
                {
                    await stream.WriteAsync(head, 0, head.Length);
                    await stream.WriteAsync(body, 0, stall ? body.Length / 2 : body.Length);
                    await stream.FlushAsync();
                }
                catch (IOException)
                {
                    // The client stopped reading (a refused download).
                    return;
                }

                if (stall)
                {
                    try
                    {
                        await stream.ReadAsync(new byte[1], 0, 1).WaitAsync(TimeSpan.FromSeconds(10));
                    }
                    catch (Exception e) when (e is IOException || e is TimeoutException || e is ObjectDisposedException)
                    {
                    }
                }
            }
        }

        private ReleaseAsset Asset(string sha256, long size) => new ReleaseAsset
        {
            Name = "OpenLamina-Windows-Installer.exe",
            DownloadUrl = $"http://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}/setup.exe",
            Sha256 = sha256,
            Size = size,
        };

        private string Destination => Path.Combine(_folder, "1.0.0", "OpenLamina-Windows-Installer.exe");

        [Fact]
        public async Task AFileWithTheListedDigestIsKeptAndReturnedOpen()
        {
            byte[] body = Encoding.ASCII.GetBytes("the setup");
            Task server = Serve(body);
            using (FileStream file = await Updater.DownloadAsync(Asset(Sha256Hex(body), body.Length), Destination, null))
            {
                var read = new byte[body.Length + 1];
                Assert.Equal(body.Length, file.Read(read, 0, read.Length));
                Assert.Equal(body, read.Take(body.Length).ToArray());
            }

            await server.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.False(File.Exists(Destination + ".part"));
        }

        [Fact]
        public async Task AFileWithAnotherDigestIsRefusedAndDeleted()
        {
            byte[] body = Encoding.ASCII.GetBytes("not the setup");
            Task server = Serve(body);
            await Assert.ThrowsAsync<InvalidDataException>(() =>
                Updater.DownloadAsync(Asset(new string('0', 64), body.Length), Destination, null));
            await server.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.False(File.Exists(Destination));
            Assert.False(File.Exists(Destination + ".part"));
        }

        [Fact]
        public async Task AFileLargerThanListedIsRefused()
        {
            byte[] body = new byte[200000];
            Task server = Serve(body);
            await Assert.ThrowsAsync<InvalidDataException>(() =>
                Updater.DownloadAsync(Asset(Sha256Hex(body), 1000), Destination, null));
            await server.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.False(File.Exists(Destination));
            Assert.False(File.Exists(Destination + ".part"));
        }

        [Fact]
        public async Task ADownloadThatStopsArrivingIsGivenUp()
        {
            byte[] body = new byte[1000];
            Task server = Serve(body, stall: true);
            Task<FileStream> download = Updater.DownloadAsync(Asset(Sha256Hex(body), body.Length), Destination, null,
                TimeSpan.FromSeconds(1));
            Assert.Same(download, await Task.WhenAny(download, Task.Delay(TimeSpan.FromSeconds(8))));
            await Assert.ThrowsAsync<IOException>(() => download);
            await server.WaitAsync(TimeSpan.FromSeconds(10));
            Assert.False(File.Exists(Destination));
            Assert.False(File.Exists(Destination + ".part"));
        }
    }

    public class IniFileTests : IDisposable
    {
        private readonly string _folder = Path.Combine(Path.GetTempPath(), "oc-tests-" + Guid.NewGuid().ToString("N"));

        public void Dispose()
        {
            if (Directory.Exists(_folder))
                Directory.Delete(_folder, true);
        }

        private const string UsersFile = "; my settings\r\n" +
                                         "handColour=#ffff00\r\n" +
                                         "haptics = off   ; too strong\r\n" +
                                         "futureOption=7\r\n" +
                                         "\r\n" +
                                         "[other]\r\n" +
                                         "laserMouse=true\r\n";

        private static string Changed(string text, Action<IniFile> change)
        {
            IniFile ini = IniFile.Parse(text);
            change(ini);
            return ini.ToText();
        }

        [Theory]
        [InlineData("")]
        [InlineData(UsersFile)]
        [InlineData("ï»¿# café\nhaptics=off")]
        [InlineData("no line end\r\n\r\n  \r\n[a\n=\n")]
        public void AFileComesBackAsItWas(string text)
        {
            Assert.Equal(text, IniFile.Parse(text).ToText());
        }

        [Fact]
        public void ReadsWhatTheLibraryReads()
        {
            IniFile ini = IniFile.Parse(UsersFile + "[default]\nkeyboard : on\nhaptics=on\n haptics=continued\n" +
                                        "#drawBounds=true\nownFrames=\nmirrorWindow=;x ;y\n");
            Assert.Equal("#ffff00", ini.Get("handColour"));
            Assert.Equal("on", ini.Get("keyboard"));
            Assert.Equal("haptics=continued", ini.Get("haptics")); // an indented line is the last name's value
            Assert.Null(ini.Get("laserMouse")); // in a section the library doesn't read
            Assert.Null(ini.Get("drawBounds"));
            Assert.Null(ini.Get("Keyboard"));
            Assert.Equal("", ini.Get("ownFrames"));
            Assert.Equal(";x", ini.Get("mirrorWindow"));
        }

        [Fact]
        public void SetChangesOnlyTheValue()
        {
            Assert.Equal(UsersFile.Replace("haptics = off   ; too strong", "haptics = true   ; too strong"),
                Changed(UsersFile, ini => ini.Set("haptics", "true")));
            Assert.Equal("a=1\nhaptics=false ; later\n",
                Changed("a=1\nhaptics= ; later\n", ini => ini.Set("haptics", "false")));
            Assert.Equal("haptics=on\nhaptics:false\n",
                Changed("haptics=on\nhaptics:off\n", ini => ini.Set("haptics", "false")));
        }

        [Fact]
        public void ANewOptionGoesWhereTheLibraryReadsIt()
        {
            Assert.Equal(UsersFile.Replace("futureOption=7\r\n", "futureOption=7\r\nlaserMouse=true\r\n"),
                Changed(UsersFile, ini => ini.Set("laserMouse", "true")));
            Assert.Equal("laserMouse=true\r\n", Changed("", ini => ini.Set("laserMouse", "true")));
            Assert.Equal("a=1\nlaserMouse=true\n", Changed("a=1\n", ini => ini.Set("laserMouse", "true")));
            Assert.Equal("a=1\r\nlaserMouse=true\r\n", Changed("a=1", ini => ini.Set("laserMouse", "true")));
            Assert.Equal("[x]\na=1\n[default]\nb=2\nlaserMouse=true\n\n[y]\n",
                Changed("[x]\na=1\n[default]\nb=2\n\n[y]\n", ini => ini.Set("laserMouse", "true")));
            Assert.Equal("ï»¿laserMouse=true\n[x]\n",
                Changed("ï»¿[x]\n", ini => ini.Set("laserMouse", "true")));
        }

        [Fact]
        public void RemoveTakesOutEveryLineThatSetsIt()
        {
            Assert.Equal("; c\na=1\n[other]\nkeyboardLayout=uk\n",
                Changed("; c\nkeyboardLayout=us\na=1\nkeyboardLayout=uk\n  uk\n[other]\nkeyboardLayout=uk\n",
                    ini => ini.Remove("keyboardLayout")));
        }

        [Fact]
        public void SavesBytesOfAnyEncodingUnchanged()
        {
            Directory.CreateDirectory(_folder);
            string file = Path.Combine(_folder, IniFile.FileName);
            byte[] before =
            {
                0xEF, 0xBB, 0xBF, (byte)';', 0xC3, 0xA9, 0xE9, 0x80, (byte)'\n', (byte)'a', (byte)'=', (byte)'1',
            };
            File.WriteAllBytes(file, before);

            IniFile ini = IniFile.Load(file);
            ini.Save(file);
            Assert.Equal(before, File.ReadAllBytes(file));

            ini.Set("a", "2");
            ini.Save(file);
            before[before.Length - 1] = (byte)'2';
            Assert.Equal(before, File.ReadAllBytes(file));
            Assert.Equal(new[] { file }, Directory.GetFiles(_folder));
        }

        [Fact]
        public void TheFileInAFolderIsTheOneTheLibraryReads()
        {
            Directory.CreateDirectory(_folder);
            string file = Path.Combine(_folder, IniFile.FileName);
            string legacy = Path.Combine(_folder, IniFile.LegacyFileName);
            Assert.Equal(file, IniFile.InFolder(_folder));
            Assert.Null(IniFile.Load(file).Get("haptics"));
            File.WriteAllText(legacy, "haptics=off\n");
            Assert.Equal(legacy, IniFile.InFolder(_folder));
            File.WriteAllText(file, "");
            Assert.Equal(file, IniFile.InFolder(_folder));
        }
    }

    public class SettingsTests
    {
        private static Setting Named(string name) => Settings.All.Single(s => s.Name == name);

        [Theory]
        [InlineData("haptics", null, "true")]
        [InlineData("haptics", "OFF", "false")]
        [InlineData("haptics", "Disabled", "false")]
        [InlineData("haptics", "maybe", "true")]
        [InlineData("laserMouse", "Enabled", "true")]
        [InlineData("laserMouse", "", "true")]
        [InlineData("laserMouse", "off", "false")]
        [InlineData("dashboardButton", "Menu_Long", "menu_long")]
        [InlineData("dashboardButton", "grip", "system")]
        [InlineData("dashboardButton", "OFF", "off")]
        [InlineData("keyboardLayout", null, "")]
        [InlineData("keyboardLayout", "UK", "uk")]
        [InlineData("supersampleRatio", "1.45", "1.45")]
        [InlineData("supersampleRatio", "2", "2.0")]
        [InlineData("supersampleRatio", "big", "1.0")]
        public void ReadsValuesAsTheLibraryDoes(string name, string raw, string value)
        {
            Assert.Equal(value, Named(name).Read(raw));
        }

        /// <summary>Each option of the window is one the library reads, with the library's default and values.</summary>
        [Fact]
        public void MatchesTheLibrarysOptions()
        {
            string folder = AppContext.BaseDirectory;
            while (folder != null && !File.Exists(Path.Combine(folder, "OpenOVR", "Misc", "Config.h")))
                folder = Path.GetDirectoryName(folder);
            Assert.NotNull(folder);
            string header = File.ReadAllText(Path.Combine(folder, "OpenOVR", "Misc", "Config.h"));
            string source = File.ReadAllText(Path.Combine(folder, "OpenOVR", "Misc", "Config.cpp"));

            Assert.Equal(Settings.All.Count, Settings.All.Select(s => s.Name).Distinct().Count());
            foreach (Setting setting in Settings.All)
            {
                Assert.Matches(@"CFGOPT\(\w+, " + setting.Name + @"\)", source);
                var field = System.Text.RegularExpressions.Regex.Match(header,
                    @"^\s*(?:bool|float|std::string) " + setting.Name + @"(?: = ([^;]+))?;",
                    System.Text.RegularExpressions.RegexOptions.Multiline);
                Assert.True(field.Success, setting.Name);
                string value = field.Groups[1].Value.Trim('"');
                Assert.Equal(setting.Default, setting.Kind == SettingKind.Number ? value.TrimEnd('f') : value);
                Assert.Equal(setting.Default, setting.Read(null));
                foreach (string choice in setting.Choices.Select(c => c.Key).Where(c => c.Length > 0))
                    Assert.Contains("\"" + choice + "\"", source);
            }
        }
    }
}
