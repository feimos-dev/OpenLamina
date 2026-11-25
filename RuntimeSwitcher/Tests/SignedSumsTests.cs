using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using RuntimeSwitcher.Core;
using Xunit;

namespace RuntimeSwitcher.Tests
{
    public class SignedSumsTests
    {
        private static readonly byte[] Sums = Encoding.UTF8.GetBytes(
            "1111111111111111111111111111111111111111111111111111111111111111  OpenLamina-Windows-Portable.zip\n" +
            "2222222222222222222222222222222222222222222222222222222222222222  OpenLamina-Windows-Installer.exe\n");

        private const string Setup = "OpenLamina-Windows-Installer.exe";
        private const string SetupSha = "2222222222222222222222222222222222222222222222222222222222222222";

        /// <summary>A key made for the test, given as the release key is: its SubjectPublicKeyInfo in base64.</summary>
        private static (ECDsa key, string publicKey) NewKey()
        {
            ECDsa key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
            return (key, Convert.ToBase64String(key.ExportSubjectPublicKeyInfo()));
        }

        // A DER signature, as openssl dgst -sha256 -sign writes it
        private static byte[] Sign(ECDsa key, byte[] data) =>
            key.SignData(data, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence);

        [Fact]
        public void SignedChecksumsGiveTheSetupsSha256()
        {
            var (key, publicKey) = NewKey();
            using (key)
            {
                byte[] signature = Sign(key, Sums);
                Assert.Equal(SetupSha, SignedSums.Expected(Sums, signature, Setup, null, publicKey));
                Assert.Equal(SetupSha, SignedSums.Expected(Sums, signature, Setup, SetupSha, publicKey));
            }
        }

        [Fact]
        public void ChecksumsSignedWithAnotherKeyOrChangedAreRefused()
        {
            var (key, publicKey) = NewKey();
            var (other, _) = NewKey();
            using (key)
            using (other)
            {
                Assert.Throws<InvalidDataException>(() => SignedSums.Expected(Sums, Sign(other, Sums), Setup, null, publicKey));
                byte[] changed = (byte[])Sums.Clone();
                changed[70] ^= 1;
                Assert.Throws<InvalidDataException>(() => SignedSums.Expected(changed, Sign(key, Sums), Setup, null, publicKey));
                Assert.Throws<InvalidDataException>(() => SignedSums.Expected(Sums, new byte[0], Setup, null, publicKey));
                Assert.Throws<InvalidDataException>(() =>
                    SignedSums.Expected(Sums, Encoding.ASCII.GetBytes("not a signature"), Setup, null, publicKey));
            }
        }

        [Fact]
        public void AFileTheChecksumsDontListOrAnotherDigestIsRefused()
        {
            var (key, publicKey) = NewKey();
            using (key)
            {
                byte[] signature = Sign(key, Sums);
                Assert.Throws<InvalidDataException>(() =>
                    SignedSums.Expected(Sums, signature, "OpenLamina-Linux.tar.gz", null, publicKey));
                Assert.Throws<InvalidDataException>(() =>
                    SignedSums.Expected(Sums, signature, Setup, new string('3', 64), publicKey));
            }
        }

        [Fact]
        public void NothingIsInstalledWithoutAPinnedKey()
        {
            var (key, _) = NewKey();
            using (key)
            {
                InvalidDataException e = Assert.Throws<InvalidDataException>(() =>
                    SignedSums.Expected(Sums, Sign(key, Sums), Setup, null, SignedSums.Placeholder));
                Assert.Contains("no release key", e.Message);
            }
        }

        [Theory]
        // A P-384 key, a P-256 key cut short, and text
        [InlineData("MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE")]
        [InlineData("MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE")]
        [InlineData("not base64!")]
        public void OnlyAP256KeyIsTaken(string publicKey) =>
            Assert.Throws<InvalidDataException>(() => SignedSums.ParsePublicKey(publicKey));

        [Fact]
        public void DerSignaturesBecomeRAndS()
        {
            // r with its high bit set (a leading zero byte in DER), s of 31 bytes
            byte[] r = Enumerable.Repeat((byte)0x81, 32).ToArray(), s = Enumerable.Repeat((byte)0x05, 31).ToArray();
            byte[] der = new byte[] { 0x30, 2 + 33 + 2 + 31, 0x02, 33, 0x00 }.Concat(r).Concat(new byte[] { 0x02, 31 })
                .Concat(s).ToArray();
            byte[] raw = SignedSums.DerToP1363(der);
            Assert.Equal(r, raw.Take(32).ToArray());
            Assert.Equal(new byte[] { 0 }.Concat(s).ToArray(), raw.Skip(32).ToArray());
            // A negative number, a needless leading zero, a byte after the sequence, a number of 33 bytes
            Assert.Null(SignedSums.DerToP1363(new byte[] { 0x30, 6, 0x02, 1, 0x81, 0x02, 1, 0x01 }));
            Assert.Null(SignedSums.DerToP1363(new byte[] { 0x30, 7, 0x02, 2, 0x00, 0x01, 0x02, 1, 0x01 }));
            Assert.Null(SignedSums.DerToP1363(new byte[] { 0x30, 6, 0x02, 1, 0x01, 0x02, 1, 0x01, 0x00 }));
            byte[] tooLong = new byte[] { 0x30, 2 + 34 + 3, 0x02, 34, 0x00 }.Concat(Enumerable.Repeat((byte)0x81, 33))
                .Concat(new byte[] { 0x02, 1, 0x01 }).ToArray();
            Assert.Null(SignedSums.DerToP1363(tooLong));
        }

        [Fact]
        public void TheChecksumsListFindsOneFile()
        {
            string text = Encoding.UTF8.GetString(Sums);
            Assert.Equal(SetupSha, SignedSums.Find(text, Setup));
            Assert.Null(SignedSums.Find(text, "Installer.exe"));
            Assert.Null(SignedSums.Find(text + SetupSha + "  " + Setup + "\n", Setup));
        }

        [Fact]
        public async Task AReleaseWithoutSignedChecksumsIsRefused()
        {
            var (key, publicKey) = NewKey();
            key.Dispose();
            var release = new Release { Tag = "v9.0.0" };
            release.Assets.Add(new ReleaseAsset
            {
                Name = Setup,
                DownloadUrl = "https://github.com/feimos-dev/OpenLamina/releases/download/v9.0.0/" + Setup,
                Sha256 = SetupSha,
            });
            InvalidDataException e = await Assert.ThrowsAsync<InvalidDataException>(() =>
                Updater.SignedSha256Async(release, Setup, publicKey));
            Assert.Contains("no signed checksums", e.Message);
            // Nor without a key, whatever the release has
            e = await Assert.ThrowsAsync<InvalidDataException>(() =>
                Updater.SignedSha256Async(release, Setup, SignedSums.Placeholder));
            Assert.Contains("no release key", e.Message);
        }

        // The key between a PEM's lines (or the placeholder)
        private static string KeyOf(string pem) =>
            string.Concat(pem.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith("-----")));

        [Fact]
        public void TheAppAndInstallShPinTheKeyOfReleaseKeyPem()
        {
            string file = KeyOf(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "release-key.pem")));
            Assert.Equal(SignedSums.PublicKey, file);
            Match m = Regex.Match(File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "install.sh")),
                "(?m)^RELEASE_KEY='([^']*)'");
            Assert.True(m.Success, "install.sh has no RELEASE_KEY='...' line");
            Assert.Equal(file, KeyOf(m.Groups[1].Value));
            if (SignedSums.IsPinned(SignedSums.PublicKey))
                SignedSums.ParsePublicKey(SignedSums.PublicKey);
        }
    }
}
