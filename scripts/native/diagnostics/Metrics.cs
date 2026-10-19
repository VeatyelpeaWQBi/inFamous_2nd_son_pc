﻿// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace Ifss.Native {
    public static class Metrics {
        // Streaming CSV supports quoted fields and escaped quotes, without loading captures into memory.
        public static IEnumerable<string[]> Csv(TextReader reader, char delimiter = ',') {
            var row = new List<string>(); var field = new StringBuilder(); bool quoted = false, endedQuote = false;
            int current;
            while ((current = reader.Read()) != -1) {
                char c = (char)current;
                if (quoted) {
                    if (c == '"') {
                        if (reader.Peek() == '"') { reader.Read(); field.Append('"'); }
                        else { quoted = false; endedQuote = true; }
                    } else field.Append(c);
                    continue;
                }
                if (c == '"' && field.Length == 0 && !endedQuote) { quoted = true; continue; }
                if (c == delimiter) { row.Add(field.ToString()); field.Length = 0; endedQuote = false; continue; }
                if (c == '\r' || c == '\n') {
                    if (c == '\r' && reader.Peek() == '\n') reader.Read();
                    if (row.Count == 0 && field.Length == 0 && !endedQuote) continue;
                    row.Add(field.ToString()); field.Length = 0; endedQuote = false;
                    yield return row.ToArray(); row.Clear(); continue;
                }
                if (endedQuote || c == '"') throw new InvalidDataException("Malformed CSV quoting.");
                field.Append(c);
            }
            if (quoted) throw new InvalidDataException("Truncated CSV field.");
            if (row.Count != 0 || field.Length != 0 || endedQuote) { row.Add(field.ToString()); yield return row.ToArray(); }
        }
        public static Dictionary<string,object> Summarize(TextReader input, double start, double end, string mode, Dictionary<string,object> confirmations) {
            var resolution = LauncherSettings.Find(mode);
            if (Double.IsNaN(start) || Double.IsInfinity(start) || Double.IsNaN(end) || Double.IsInfinity(end) || start < 0 || start >= end)
                throw new InvalidDataException("Invalid measurement window.");
            var frames = new List<double>(); long previousElapsed = -1, previousIndex = -1; int frameColumn = -1, elapsedColumn = -1, intervalColumn = -1;
            foreach (var row in Csv(input)) {
                if (frameColumn < 0) {
                    frameColumn = Array.IndexOf(row,"frame"); elapsedColumn = Array.IndexOf(row,"elapsed_ns"); intervalColumn = Array.IndexOf(row,"interval_ns");
                    if (frameColumn < 0 || elapsedColumn < 0 || intervalColumn < 0) throw new InvalidDataException("Required frame CSV columns are missing.");
                    continue;
                }
                if (row.Length <= Math.Max(frameColumn,Math.Max(elapsedColumn,intervalColumn))) throw new InvalidDataException("Truncated CSV row.");
                long index = Int64.Parse(row[frameColumn],CultureInfo.InvariantCulture), elapsed = Int64.Parse(row[elapsedColumn],CultureInfo.InvariantCulture), interval = Int64.Parse(row[intervalColumn],CultureInfo.InvariantCulture);
                if (previousIndex < 0) {
                    if (index != 0 || elapsed != 0 || interval != 0) throw new InvalidDataException("Frame timeline must begin at zero.");
                } else if (index != checked(previousIndex + 1) || elapsed <= previousElapsed || elapsed - previousElapsed != interval)
                    throw new InvalidDataException("Frame timeline is not contiguous or timestamps disagree.");
                if (previousElapsed >= 0 && previousElapsed >= start * 1e9 && elapsed <= end * 1e9) frames.Add(interval / 1e6);
                previousIndex = index; previousElapsed = elapsed;
            }
            if (previousElapsed < end * 1e9) throw new InvalidDataException("Recording does not cover the full requested measurement window.");
            if (frames.Count < 2) throw new InvalidDataException("Measurement contains fewer than two complete frame intervals.");
            double mean = frames.Average(), fps = 1000 / mean; frames.Sort();
            double p95 = frames[(int)Math.Ceiling(frames.Count * .95) - 1], p99 = frames[(int)Math.Ceiling(frames.Count * .99) - 1];
            object numeric = mode == "1080p" ? (object)(fps >= 59 && p95 <= 17.5) : mode == "pro4k" ? (object)(fps >= 40 && p95 <= 25) : null;
            bool visual = new [] {"scene","brightness","particles","resolution"}.All(key => Json.Bool(confirmations,key,false));
            return new Dictionary<string,object> {
                {"schema",2},{"mode",mode},{"counter_kind","guest_flip"},
                {"resolution",new Dictionary<string,object>{{"width",resolution.Width},{"height",resolution.Height}}},
                {"baseline_target_applicable",mode == "1080p" || mode == "pro4k"},
                {"frames",frames.Count},{"duration_seconds",frames.Sum()/1000},{"fps",fps},{"mean_ms",mean},{"p95_ms",p95},{"p99_ms",p99},{"max_ms",frames[frames.Count-1]},
                {"frames_over_50ms",frames.Count(x => x > 50)},{"frames_over_100ms",frames.Count(x => x > 100)},
                {"numeric_screen_pass",numeric},{"confirmations",confirmations},
                {"baseline_acceptance",Object.Equals(numeric,true) && visual ? "candidate-pass" : "not-verified"},{"hardware_present_verified",false},
                {"note","Guest flips do not prove scanout, no tearing, or a full-game stable framerate."}
            };
        }
    }
}
