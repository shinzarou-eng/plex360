// PlexRelay — pont Plex -> Xbox 360 (serveur HTTP sur TCP brut, pas d'ACL)
// /start?file=...  -> transcode (ffmpeg->mp4 si besoin puis MediaTranscoder->wmv)
// /read?id=&off=&len= -> bytes du wmv en cours d'ecriture (bloque si en retard)
// /status?id= /stop?id=
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using Windows.Media.MediaProperties;
using Windows.Media.Transcoding;
using Windows.Storage;

class Job
{
    public int Id;
    public string Source;
    public string OutPath;
    public long Written;
    public bool Done;
    public string Error;
    public CancellationTokenSource Cts = new();
}

static class Program
{
    static int _nextId;
    static readonly ConcurrentDictionary<int, Job> Jobs = new();
    static readonly string TempDir = Path.Combine(Path.GetTempPath(), "plex360");
    const int Port = 8090;

    static int Main(string[] args)
    {
        if (args.Length >= 3 && args[0] == "transcode")
        {
            var t = TranscodeAsync(args[1], args[2], CancellationToken.None);
            t.Wait();
            Console.WriteLine(t.Result ?? "OK");
            return t.Result == null ? 0 : 1;
        }
        Directory.CreateDirectory(TempDir);
        var srv = new TcpListener(IPAddress.Any, Port);
        srv.Start();
        Console.WriteLine($"PlexRelay pret sur :{Port}");
        for (;;)
        {
            var cli = srv.AcceptTcpClient();
            ThreadPool.QueueUserWorkItem(_ => Handle(cli));
        }
    }

    static async Task<string> TranscodeAsync(string src, string dst, CancellationToken ct)
    {
        try
        {
            var inFile = await StorageFile.GetFileFromPathAsync(src);
            new FileStream(dst, FileMode.Create).Dispose();
            var outFile = await StorageFile.GetFileFromPathAsync(dst);
            var profile = MediaEncodingProfile.CreateWmv(VideoEncodingQuality.HD720p);
            var tc = new MediaTranscoder();
            var prep = await tc.PrepareFileTranscodeAsync(inFile, outFile, profile);
            if (!prep.CanTranscode)
                return "CanTranscode=false reason=" + prep.FailureReason;
            await prep.TranscodeAsync().AsTask(ct);
            return null;
        }
        catch (Exception e) { return $"ex: {e.GetType().Name} hr=0x{e.HResult:X8} {e.Message}"; }
    }

    static async Task<string> PipelineAsync(Job job)
    {
        var src = job.Source;
        if (!File.Exists(src)) return "fichier introuvable: " + src;
        var err = await TranscodeAsync(src, job.OutPath, job.Cts.Token);
        if (err == null) return null;
        try { File.Delete(job.OutPath); } catch { }
        string tmpMp4 = Path.Combine(TempDir, $"job{job.Id}.mp4");
        string fargs = "-hide_banner -y -i \"" + src + "\" -map 0:v:0 -map 0:a:0? " +
            "-c:v libx264 -preset veryfast -crf 23 -c:a aac -b:a 160k " +
            "-movflags +faststart \"" + tmpMp4 + "\"";
        var psi = new System.Diagnostics.ProcessStartInfo("ffmpeg", fargs)
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true };
        var p = System.Diagnostics.Process.Start(psi);
        var ferr = await p.StandardError.ReadToEndAsync();
        try { await p.WaitForExitAsync(job.Cts.Token); }
        catch (OperationCanceledException) { TryKill(p); return "annule"; }
        if (p.ExitCode != 0 || !File.Exists(tmpMp4))
            return "ffmpeg exit=" + p.ExitCode + " " + LastLines(ferr, 3);
        err = await TranscodeAsync(tmpMp4, job.OutPath, job.Cts.Token);
        try { File.Delete(tmpMp4); } catch { }
        return err;
    }

    static void TryKill(System.Diagnostics.Process p) { try { p.Kill(); } catch { } }

    static string ProbeVideoCodec(string path)
    {
        try
        {
            var psi = new System.Diagnostics.ProcessStartInfo("ffprobe",
                $"-v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 \"{path}\"")
            { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
            var p = System.Diagnostics.Process.Start(psi);
            var s = p.StandardOutput.ReadToEnd().Trim();
            p.WaitForExit(10000);
            return s;
        }
        catch { return ""; }
    }

    static string LastLines(string s, int n)
    {
        var l = s.Split('\n', StringSplitOptions.RemoveEmptyEntries);
        return string.Join(" | ", l.Skip(Math.Max(0, l.Length - n))).Trim();
    }

    // ---- mini serveur HTTP ----
    static async void Handle(TcpClient cli)
    {
        try
        {
            using (cli)
            {
                cli.ReceiveTimeout = 15000;
                var ns = cli.GetStream();
                // lis la ligne de requete + headers (jusqu'a \r\n\r\n)
                var head = new MemoryStream();
                var b = new byte[1];
                while (head.Length < 16384)
                {
                    int n = await ns.ReadAsync(b, 0, 1);
                    if (n <= 0) return;
                    head.WriteByte(b[0]);
                    var h = head.GetBuffer();
                    if (head.Length >= 4 && h[head.Length-4]=='\r' && h[head.Length-3]=='\n'
                        && h[head.Length-2]=='\r' && h[head.Length-1]=='\n') break;
                }
                var req = Encoding.ASCII.GetString(head.ToArray());
                var line = req.Split('\n')[0].Trim();
                var parts = line.Split(' ');
                if (parts.Length < 2 || parts[0] != "GET") { Reply(cli, 400, "GET only"); return; }
                var url = parts[1];
                var path = url.Split('?')[0];
                var q = new Dictionary<string, string>();
                var qi = url.IndexOf('?');
                if (qi >= 0)
                    foreach (var kv in url.Substring(qi + 1).Split('&'))
                    {
                        var eq = kv.IndexOf('=');
                        if (eq > 0) q[kv.Substring(0, eq)] = Uri.UnescapeDataString(kv.Substring(eq + 1));
                    }

                switch (path)
                {
                    case "/start":
                    {
                        var file = q.ContainsKey("file") ? q["file"] : null;
                        if (string.IsNullOrEmpty(file)) { Reply(cli, 400, "file= requis"); return; }
                        int id = Interlocked.Increment(ref _nextId);
                        var job = new Job { Id = id, Source = file,
                            OutPath = Path.Combine(TempDir, $"job{id}.wmv") };
                        Jobs[id] = job;
                        _ = Task.Run(async () =>
                        {
                            job.Error = await PipelineAsync(job);
                            job.Done = true;
                            try { job.Written = new FileInfo(job.OutPath).Length; } catch { }
                        });
                        _ = Task.Run(async () =>
                        {
                            while (!job.Done)
                            {
                                try { job.Written = new FileInfo(job.OutPath).Length; } catch { }
                                await Task.Delay(500);
                            }
                        });
                        Reply(cli, 200, $"id={id}");
                        return;
                    }
                    case "/read":
                    {
                        int id = int.Parse(q["id"]);
                        long off = long.Parse(q["off"]);
                        int len = int.Parse(q["len"]);
                        if (!Jobs.TryGetValue(id, out var job)) { Reply(cli, 404, "job inconnu"); return; }
                        await ServeRange(cli, job, off, len);
                        return;
                    }
                    case "/status":
                    {
                        int id = int.Parse(q["id"]);
                        if (!Jobs.TryGetValue(id, out var job)) { Reply(cli, 404, "job inconnu"); return; }
                        Reply(cli, 200, $"written={job.Written} done={(job.Done ? 1 : 0)} err={job.Error}");
                        return;
                    }
                    case "/stop":
                    {
                        int id = int.Parse(q["id"]);
                        if (Jobs.TryRemove(id, out var job))
                        {
                            job.Cts.Cancel();
                            try { File.Delete(job.OutPath); } catch { }
                        }
                        Reply(cli, 200, "ok");
                        return;
                    }
                    default:
                        Reply(cli, 200, "PlexRelay: /start?file= /read?id=&off=&len= /status?id= /stop?id=");
                        return;
                }
            }
        }
        catch { try { cli.Close(); } catch { } }
    }

    static async Task ServeRange(TcpClient cli, Job job, long off, int len)
    {
        for (int i = 0; i < 500; ++i)
        {
            if (job.Written > off || job.Done) break;
            if (job.Error != null) break;
            await Task.Delay(60);
        }
        long avail = Math.Max(0, job.Written - off);
        int n = (int)Math.Min(len, avail);
        if (n <= 0) { Reply(cli, 200, ""); return; }
        var buf = new byte[n];
        int got = 0;
        for (int t = 0; t < 20 && got == 0; ++t)
        {
            try
            {
                using (var fs = new FileStream(job.OutPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    fs.Position = off;
                    got = await fs.ReadAsync(buf, 0, n);
                }
            }
            catch { await Task.Delay(100); }
        }
        if (got <= 0) { Reply(cli, 200, ""); return; }
        var hdr = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 200 OK\r\nContent-Length: {got}\r\nConnection: close\r\n\r\n");
        await cli.GetStream().WriteAsync(hdr, 0, hdr.Length);
        await cli.GetStream().WriteAsync(buf, 0, got);
        cli.Close();
    }

    static void Reply(TcpClient cli, int code, string s)
    {
        var body = Encoding.UTF8.GetBytes(s ?? "");
        var reason = code == 200 ? "OK" : code == 404 ? "Not Found" : "Error";
        var hdr = Encoding.ASCII.GetBytes(
            $"HTTP/1.1 {code} {reason}\r\nContent-Length: {body.Length}\r\nConnection: close\r\n\r\n");
        try
        {
            cli.GetStream().Write(hdr, 0, hdr.Length);
            cli.GetStream().Write(body, 0, body.Length);
            cli.Close();
        }
        catch { }
    }
}
