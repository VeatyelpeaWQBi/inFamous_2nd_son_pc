// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace Ifss.Native {
    public static class Json {
        public static Dictionary<string, object> Object(object value) {
            var result = value as Dictionary<string, object>;
            if (result == null) throw new InvalidDataException("Expected a JSON object.");
            return result;
        }
        public static object Get(Dictionary<string, object> obj, string key, object fallback) {
            object value; return obj.TryGetValue(key, out value) ? value : fallback;
        }
        public static string Text(Dictionary<string, object> obj, string key, string fallback) {
            var value = Get(obj, key, fallback) as string;
            if (value == null) throw new InvalidDataException("Expected a string: " + key);
            return value;
        }
        public static bool Bool(Dictionary<string, object> obj, string key, bool fallback) {
            var value = Get(obj, key, fallback);
            if (!(value is bool)) throw new InvalidDataException("Expected a boolean: " + key);
            return (bool)value;
        }
        public static long Integer(object value) {
            if (!(value is int) && !(value is long)) throw new InvalidDataException("Expected an integer.");
            return Convert.ToInt64(value, CultureInfo.InvariantCulture);
        }
        public static Dictionary<string, object> Group(Dictionary<string, object> obj, string key) {
            if (!obj.ContainsKey(key)) obj[key] = new Dictionary<string, object>();
            return Object(obj[key]);
        }
        static JavaScriptSerializer Serializer() {
            return new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024, RecursionLimit = 128 };
        }
        public static string Encode(object value) { return Serializer().Serialize(value); }
        public static object Decode(string text) { return Serializer().DeserializeObject(text); }
        public static Dictionary<string, object> Read(string path) {
            if (new FileInfo(path).Length > 16 * 1024 * 1024) throw new InvalidDataException("JSON exceeds supported size.");
            return Object(Decode(File.ReadAllText(path, new UTF8Encoding(false, true))));
        }
    }
    public sealed class ProjectPaths {
        public readonly string Root;
        public ProjectPaths(string root) {
            Root = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        }
        public static bool Inside(string root, string path) {
            return Path.GetFullPath(path).StartsWith(Path.GetFullPath(root).TrimEnd('\\', '/') + Path.DirectorySeparatorChar,
                                                    StringComparison.OrdinalIgnoreCase);
        }
        public string Resolve(string value) {
            if (String.IsNullOrWhiteSpace(value) || Path.IsPathRooted(value) || value.IndexOf(':') >= 0)
                throw new InvalidDataException("Stored paths must be project-relative.");
            return Path.GetFullPath(Path.Combine(Root, value));
        }
        public string Child(string value) {
            var path = Resolve(value);
            if (!Inside(Root, path)) throw new InvalidDataException("Path escaped the project directory.");
            for (var entry = path; !String.Equals(entry, Root, StringComparison.OrdinalIgnoreCase); entry = Path.GetDirectoryName(entry)) {
                try {
                    if ((File.GetAttributes(entry) & FileAttributes.ReparsePoint) != 0)
                        throw new InvalidDataException("Project data path contains a link.");
                } catch (FileNotFoundException) { } catch (DirectoryNotFoundException) { }
            }
            return path;
        }
        public string Relative(string path) {
            var source = new Uri(Root + Path.DirectorySeparatorChar);
            var target = new Uri(Path.GetFullPath(path));
            if (!String.Equals(source.Scheme, target.Scheme, StringComparison.OrdinalIgnoreCase) ||
                !String.Equals(Path.GetPathRoot(Root), Path.GetPathRoot(path), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Relative paths cannot cross drives.");
            return Uri.UnescapeDataString(source.MakeRelativeUri(target).ToString());
        }
        public void WriteText(string path, string text) {
            path = Child(Relative(path));
            if (!Inside(Child("state"), path) && !Inside(Child("doc"), path))
                throw new InvalidDataException("Native tools may write only below state/ or doc/.");
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            var pending = path + "." + Guid.NewGuid().ToString("N") + ".pending";
            try {
                File.WriteAllText(pending, text, new UTF8Encoding(false));
                for (int attempt = 0; ; ++attempt) {
                    try {
                        Child(Relative(path));
                        if (File.Exists(path)) File.Replace(pending, path, null);
                        else File.Move(pending, path);
                        break;
                    } catch (IOException error) {
                        int code = error.HResult & 0xffff;
                        if (attempt >= 6 || (code != 32 && code != 33 && code != 1175)) throw;
                        Thread.Sleep(Math.Min(400, 50 << attempt));
                    }
                }
            } finally { if (File.Exists(pending)) File.Delete(pending); }
        }
        public void WriteJson(string path, object value) { WriteText(path, Json.Encode(value) + "\n"); }
        public static string Hash(byte[] data) {
            using (var sha = SHA256.Create()) return Hex(sha.ComputeHash(data));
        }
        public static string HashFile(string path) {
            using (var stream = File.OpenRead(path))
            using (var sha = SHA256.Create()) return Hex(sha.ComputeHash(stream));
        }
        public static string Hex(byte[] bytes) { return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant(); }
        public static byte[] Unhex(string text) {
            text = System.Text.RegularExpressions.Regex.Replace(text, @"\s", "");
            if ((text.Length & 1) != 0) throw new InvalidDataException("Invalid hexadecimal payload.");
            var result = new byte[text.Length / 2];
            for (int i = 0; i < result.Length; ++i) result[i] = Byte.Parse(text.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            return result;
        }
    }
}
