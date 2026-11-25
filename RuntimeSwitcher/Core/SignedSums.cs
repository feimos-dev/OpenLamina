using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace RuntimeSwitcher.Core
{
    /// <summary>
    /// A release's SHA256SUMS.txt and its signature, SHA256SUMS.txt.sig: what the release job made with
    /// <c>openssl dgst -sha256 -sign</c> and the release key, an ECDSA P-256 signature in DER. A file of a release is
    /// installed only with the SHA-256 these list, and only if they are signed with the key pinned here.
    /// </summary>
    public static class SignedSums
    {
        public const string SumsName = "SHA256SUMS.txt";
        public const string SignatureName = "SHA256SUMS.txt.sig";

        /// <summary>What <see cref="PublicKey"/> is until the release key is made: then no update is installed.</summary>
        public const string Placeholder = "REPLACE-WITH-THE-RELEASE-PUBLIC-KEY";

        /// <summary>
        /// The release key's public half: the base64 between the PEM lines of installer/release-key.pem (its
        /// SubjectPublicKeyInfo). install.sh pins the same key; a test checks the three agree.
        /// </summary>
        public const string PublicKey = Placeholder;

        public static bool IsPinned(string publicKey) => publicKey != null && publicKey != Placeholder;

        // SubjectPublicKeyInfo of an uncompressed P-256 point: SEQUENCE { SEQUENCE { id-ecPublicKey, prime256v1 },
        // BIT STRING { 04 X Y } }, X and Y 32 bytes each
        private static readonly byte[] P256Prefix =
        {
            0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48,
            0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04,
        };

        /// <summary>
        /// The key's X and Y from its base64 SubjectPublicKeyInfo (spaces and line breaks ignored), or an
        /// InvalidDataException for anything but a P-256 key.
        /// </summary>
        public static ECParameters ParsePublicKey(string base64)
        {
            byte[] der;
            try
            {
                der = Convert.FromBase64String(Regex.Replace(base64 ?? "", @"\s", ""));
            }
            catch (FormatException)
            {
                throw new InvalidDataException("The release key isn't base64.");
            }

            if (der.Length != P256Prefix.Length + 64 || !der.Take(P256Prefix.Length).SequenceEqual(P256Prefix))
                throw new InvalidDataException("The release key isn't an ECDSA P-256 public key.");
            return new ECParameters
            {
                Curve = ECCurve.NamedCurves.nistP256,
                Q = new ECPoint
                {
                    X = der.Skip(P256Prefix.Length).Take(32).ToArray(),
                    Y = der.Skip(P256Prefix.Length + 32).Take(32).ToArray(),
                },
            };
        }

        /// <summary>
        /// A DER ECDSA signature (SEQUENCE { INTEGER r, INTEGER s }) as the 64 bytes r and s that .NET verifies; null if
        /// it isn't one, or either number doesn't fit 32 bytes.
        /// </summary>
        public static byte[] DerToP1363(byte[] der)
        {
            if (der == null || der.Length < 8 || der[0] != 0x30)
                return null;
            int at = 1;
            if (!ReadLength(der, ref at, out int seqLength) || at + seqLength != der.Length)
                return null;
            var result = new byte[64];
            for (int i = 0; i < 2; i++)
            {
                if (at >= der.Length || der[at++] != 0x02 || !ReadLength(der, ref at, out int length) || length < 1 ||
                    at + length > der.Length)
                    return null;
                // A positive number: a leading zero byte only before a byte with its high bit set
                int start = at, end = at + length;
                if ((der[start] & 0x80) != 0 || (length > 1 && der[start] == 0 && (der[start + 1] & 0x80) == 0))
                    return null;
                while (start < end && der[start] == 0)
                    start++;
                if (end - start > 32)
                    return null;
                Array.Copy(der, start, result, i * 32 + 32 - (end - start), end - start);
                at = end;
            }

            return at == der.Length ? result : null;
        }

        // A DER length of at most two bytes after its first
        private static bool ReadLength(byte[] der, ref int at, out int length)
        {
            length = 0;
            if (at >= der.Length)
                return false;
            byte first = der[at++];
            if (first < 0x80)
            {
                length = first;
                return true;
            }

            int count = first & 0x7f;
            if (count < 1 || count > 2 || at + count > der.Length)
                return false;
            for (int i = 0; i < count; i++)
                length = (length << 8) | der[at++];
            return length >= 0x80;
        }

        /// <summary>Whether <paramref name="signature"/> (DER) signs <paramref name="sums"/> with the key.</summary>
        public static bool Verify(byte[] sums, byte[] signature, string publicKey)
        {
            byte[] raw = DerToP1363(signature);
            if (raw == null)
                return false;
            using (ECDsa key = ECDsa.Create(ParsePublicKey(publicKey)))
                return key.VerifyData(sums, raw, HashAlgorithmName.SHA256);
        }

        /// <summary>
        /// The SHA-256 (lower-case hex) SHA256SUMS.txt lists for <paramref name="fileName"/>, as sha256sum writes it
        /// ("&lt;hex&gt;  &lt;name&gt;"); null if it lists none, or more than one.
        /// </summary>
        public static string Find(string sums, string fileName)
        {
            List<string> found = (sums ?? "").Split('\n')
                .Select(line => Regex.Match(line.TrimEnd('\r'), @"^([0-9a-fA-F]{64}) [ *](.+)$"))
                .Where(m => m.Success && m.Groups[2].Value == fileName)
                .Select(m => m.Groups[1].Value.ToLowerInvariant())
                .ToList();
            return found.Count == 1 ? found[0] : null;
        }

        /// <summary>
        /// The SHA-256 of <paramref name="fileName"/> from a release's signed checksums, after checking the signature
        /// with <paramref name="publicKey"/>. Throws an InvalidDataException saying why for anything else: no key
        /// pinned, a signature that doesn't match, the file not listed, or a SHA-256 other than the one GitHub lists.
        /// </summary>
        public static string Expected(byte[] sums, byte[] signature, string fileName, string githubSha256,
            string publicKey)
        {
            if (!IsPinned(publicKey))
                throw new InvalidDataException("This build of OpenLamina has no release key to check updates with. " +
                                               "Download the new release from the releases page.");
            if (!Verify(sums, signature, publicKey))
                throw new InvalidDataException(
                    $"The release was refused: its {SumsName} isn't signed with OpenLamina's release key.");
            string sha = Find(Encoding.UTF8.GetString(sums), fileName);
            if (sha == null)
                throw new InvalidDataException($"The release was refused: its signed {SumsName} doesn't list {fileName}.");
            if (githubSha256 != null && githubSha256 != sha)
                throw new InvalidDataException(
                    $"The release was refused: GitHub lists another SHA-256 for {fileName} than its signed {SumsName}.");
            return sha;
        }
    }
}
