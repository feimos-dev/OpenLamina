using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace RuntimeSwitcher.Core
{
    /// <summary>
    /// Asks GitHub for the fork's releases and downloads a release's file, checked against the SHA-256 its signed
    /// SHA256SUMS.txt lists (<see cref="SignedSums"/>).
    /// </summary>
    public static class Updater
    {
        private static readonly Lazy<HttpClient> Client = new Lazy<HttpClient>(() =>
        {
            var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
            // GitHub's API refuses requests without a User-Agent.
            client.DefaultRequestHeaders.UserAgent.ParseAdd("OpenLamina-updater");
            client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
            return client;
        });

        public static async Task<List<Release>> GetReleasesAsync()
        {
            using (HttpResponseMessage response = await Client.Value.GetAsync(Releases.ApiUrl).ConfigureAwait(false))
            {
                if (!response.IsSuccessStatusCode)
                    throw new HttpRequestException($"GitHub answered {(int)response.StatusCode} {response.ReasonPhrase}.");
                return Releases.Parse(await response.Content.ReadAsStringAsync().ConfigureAwait(false));
            }
        }

        /// <summary>A download that receives nothing for this long is given up.</summary>
        public static readonly TimeSpan DefaultReadTimeout = TimeSpan.FromSeconds(60);

        /// <summary>The most a release's SHA256SUMS.txt or its signature may be.</summary>
        public const int MaxSumsBytes = 64 * 1024;

        /// <summary>
        /// The SHA-256 of <paramref name="release"/>'s <paramref name="assetName"/> from its SHA256SUMS.txt, once its
        /// signature (SHA256SUMS.txt.sig) is checked with <paramref name="publicKey"/> (default: the pinned
        /// <see cref="SignedSums.PublicKey"/>). Throws an InvalidDataException saying why when the release has no such
        /// files on github.com or they don't check out (<see cref="SignedSums.Expected"/>).
        /// </summary>
        public static async Task<string> SignedSha256Async(Release release, string assetName,
            string publicKey = SignedSums.PublicKey, TimeSpan readTimeout = default)
        {
            SignedSums.RequirePinned(publicKey);
            ReleaseAsset asset = release.Asset(assetName);
            ReleaseAsset sums = release.Asset(SignedSums.SumsName), signature = release.Asset(SignedSums.SignatureName);
            if (asset == null)
                throw new InvalidDataException($"The release has no {assetName}.");
            if (sums == null || signature == null || !Releases.IsGitHubUrl(sums.DownloadUrl) ||
                !Releases.IsGitHubUrl(signature.DownloadUrl))
                throw new InvalidDataException(
                    $"The release was refused: it has no signed checksums ({SignedSums.SumsName} and {SignedSums.SignatureName}).");
            byte[] sumsBytes = await GetSmallAsync(sums, readTimeout).ConfigureAwait(false);
            byte[] signatureBytes = await GetSmallAsync(signature, readTimeout).ConfigureAwait(false);
            return SignedSums.Expected(release.Tag, sumsBytes, signatureBytes, assetName, asset.Sha256, publicKey);
        }

        // A file of at most MaxSumsBytes, in memory
        private static async Task<byte[]> GetSmallAsync(ReleaseAsset asset, TimeSpan readTimeout)
        {
            if (readTimeout <= TimeSpan.Zero)
                readTimeout = DefaultReadTimeout;
            using (HttpResponseMessage response = await Client.Value
                       .GetAsync(asset.DownloadUrl, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
            {
                response.EnsureSuccessStatusCode();
                using (Stream input = await response.Content.ReadAsStreamAsync().ConfigureAwait(false))
                using (var output = new MemoryStream())
                {
                    var buffer = new byte[8192];
                    int read;
                    while ((read = await ReadWithin(input, buffer, response, readTimeout).ConfigureAwait(false)) > 0)
                    {
                        if (output.Length + read > MaxSumsBytes)
                            throw new InvalidDataException($"{asset.Name} was refused: it is larger than {MaxSumsBytes} bytes.");
                        output.Write(buffer, 0, read);
                    }

                    return output.ToArray();
                }
            }
        }

        /// <summary>
        /// Downloads <paramref name="asset"/> to <paramref name="destination"/> and keeps it only if its SHA-256 is
        /// <paramref name="sha256"/> (from <see cref="SignedSha256Async"/>). <paramref name="progress"/> gets 0–100.
        /// Returns the file open for reading, with only reading shared, so it can't be changed or replaced between the
        /// check and its use; dispose it after. Throws an IOException when nothing arrives for
        /// <paramref name="readTimeout"/> (default <see cref="DefaultReadTimeout"/>), and an InvalidDataException for
        /// more bytes than GitHub lists.
        /// </summary>
        public static async Task<FileStream> DownloadAsync(ReleaseAsset asset, string sha256, string destination,
            IProgress<int> progress, TimeSpan readTimeout = default)
        {
            if (sha256 == null)
                throw new InvalidDataException($"No checked SHA-256 for {asset.Name}.");
            if (readTimeout <= TimeSpan.Zero)
                readTimeout = DefaultReadTimeout;
            Directory.CreateDirectory(Path.GetDirectoryName(destination));
            string part = destination + ".part";

            try
            {
                using (HttpResponseMessage response = await Client.Value
                           .GetAsync(asset.DownloadUrl, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
                {
                    response.EnsureSuccessStatusCode();
                    long total = response.Content.Headers.ContentLength ?? asset.Size;
                    using (Stream input = await response.Content.ReadAsStreamAsync().ConfigureAwait(false))
                    using (var output = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None))
                    {
                        var buffer = new byte[81920];
                        long done = 0;
                        int read;
                        while ((read = await ReadWithin(input, buffer, response, readTimeout).ConfigureAwait(false)) > 0)
                        {
                            done += read;
                            if (asset.Size > 0 && done > asset.Size)
                                throw new InvalidDataException(
                                    $"{asset.Name} was refused: it is larger than the {asset.Size} bytes GitHub lists.");
                            await output.WriteAsync(buffer, 0, read).ConfigureAwait(false);
                            if (total > 0)
                                progress?.Report((int)Math.Min(100, done * 100 / total));
                        }
                    }
                }
            }
            catch
            {
                TryDelete(part);
                throw;
            }

            if (File.Exists(destination))
                File.Delete(destination);
            File.Move(part, destination);

            // Checked through the handle that is returned, so what was checked is what runs.
            var file = new FileStream(destination, FileMode.Open, FileAccess.Read, FileShare.Read);
            try
            {
                string actual = Sha256Of(file);
                if (actual == sha256)
                {
                    file.Position = 0;
                    return file;
                }

                file.Dispose();
                File.Delete(destination);
                throw new InvalidDataException(
                    $"{asset.Name} was refused: its SHA-256 is {actual}, but the signed checksums list {sha256}.");
            }
            catch
            {
                file.Dispose();
                throw;
            }
        }

        /// <summary>
        /// One read, given up after <paramref name="timeout"/> without data: the response is closed, which ends the
        /// read on any platform (a cancellation token isn't honoured by every network stream once the read started).
        /// </summary>
        private static async Task<int> ReadWithin(Stream input, byte[] buffer, HttpResponseMessage response,
            TimeSpan timeout)
        {
            Task<int> read = input.ReadAsync(buffer, 0, buffer.Length);
            using (var stopDelay = new CancellationTokenSource())
            {
                if (await Task.WhenAny(read, Task.Delay(timeout, stopDelay.Token)).ConfigureAwait(false) == read)
                {
                    stopDelay.Cancel();
                    return await read.ConfigureAwait(false);
                }
            }

            response.Dispose();
            // The read then fails; its exception is observed here and replaced by the one below.
            _ = read.ContinueWith(t => _ = t.Exception, TaskContinuationOptions.OnlyOnFaulted);
            throw new IOException($"The download stopped: nothing arrived for {timeout.TotalSeconds:0} seconds.");
        }

        private static void TryDelete(string file)
        {
            try
            {
                File.Delete(file);
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
                // Overwritten by the next download.
            }
        }

        /// <summary>Deletes the folders in <paramref name="folder"/> other than <paramref name="keep"/>, as far as it can.</summary>
        public static void DeleteOtherFolders(string folder, string keep)
        {
            try
            {
                foreach (string old in Directory.GetDirectories(folder))
                {
                    if (string.Equals(Path.GetFileName(old), keep, StringComparison.OrdinalIgnoreCase))
                        continue;
                    try
                    {
                        Directory.Delete(old, true);
                    }
                    catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
                    {
                        // In use or not ours: left for next time.
                    }
                }
            }
            catch (Exception e) when (e is IOException || e is UnauthorizedAccessException)
            {
                // No folder yet, or it can't be listed.
            }
        }

        public static string Sha256Of(string file)
        {
            using (FileStream stream = File.OpenRead(file))
                return Sha256Of(stream);
        }

        public static string Sha256Of(Stream stream)
        {
            using (SHA256 sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
        }
    }
}
