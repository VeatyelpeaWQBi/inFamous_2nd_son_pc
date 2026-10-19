// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace Ifss.Native {
    public static class DiagnosticData {
        public static Dictionary<string,object> Map(params object[] entries) {
            if(entries.Length%2!=0)throw new ArgumentException("Expected key/value pairs.");
            var result=new Dictionary<string,object>();for(int i=0;i<entries.Length;i+=2)result.Add((string)entries[i],entries[i+1]);return result;
        }
        public static object Get(Dictionary<string,object> row,string key,object fallback=null){object value;return row.TryGetValue(key,out value)?value:fallback;}
        public static string Text(Dictionary<string,object> row,string key,string fallback=""){object value=Get(row,key,null);return value==null?fallback:Json.Text(row,key,fallback);}
        public static long Int(Dictionary<string,object> row,string key,long fallback=0){return Json.Integer(Get(row,key,fallback));}
        public static double Number(object value){return Convert.ToDouble(value,CultureInfo.InvariantCulture);}
        public static bool Flag(Dictionary<string,object> row,string key){return Object.Equals(Get(row,key,false),true);}
        public static bool Finite(double value){return !Double.IsNaN(value)&&!Double.IsInfinity(value);}
        public static IEnumerable<object> Array(object value){return value==null?Enumerable.Empty<object>():((IEnumerable)value).Cast<object>();}
        public static double Utc(string value){return (DateTimeOffset.Parse(value,CultureInfo.InvariantCulture,DateTimeStyles.None).UtcDateTime.Ticks-621355968000000000L)/1e7;}
        public static Dictionary<string,object> Load(string directory,string name){return Json.Read(Path.Combine(directory,name));}
        public static IEnumerable<string[]> Rows(string path,char delimiter=',') {
            using(var reader=new StreamReader(path,Encoding.UTF8,true)) {
                int width=-1;foreach(var row in Metrics.Csv(reader,delimiter)){
                    if(width<0)width=row.Length;
                    if(row.Length!=width || row.Any(value=>value.Length>16*1024*1024))throw new InvalidDataException(Path.GetFileName(path)+": incomplete/changing columns or oversized field.");
                    yield return row;
                }
                if(width<0)throw new InvalidDataException(Path.GetFileName(path)+": empty table.");
            }
        }
        public static long ParseInt(string value){return Int64.Parse(value,CultureInfo.InvariantCulture);}
        public static ulong ParseUnsigned(string value){return value.StartsWith("0x",StringComparison.OrdinalIgnoreCase)?UInt64.Parse(value.Substring(2),NumberStyles.HexNumber,CultureInfo.InvariantCulture):UInt64.Parse(value,CultureInfo.InvariantCulture);}
        public static double ParseNumber(string value){return Double.Parse(value,NumberStyles.Float,CultureInfo.InvariantCulture);}
        public static void Check(bool condition,string message){if(!condition)throw new InvalidDataException(message);}
        public static byte[] ReadAt(Stream stream,long offset,int count) {
            Check(offset>=0 && count>=0 && offset<=stream.Length && count<=stream.Length-offset,"Truncated binary structure.");
            stream.Position=offset;var bytes=new byte[count];int done=0;while(done<count){int read=stream.Read(bytes,done,count-done);if(read==0)throw new EndOfStreamException();done+=read;}return bytes;
        }
        public static IEnumerable<string> Files(string directory,string pattern){return Directory.Exists(directory)?Directory.EnumerateFiles(directory,pattern,SearchOption.AllDirectories):Enumerable.Empty<string>();}
        public static void AddCount(Dictionary<string,long> values,string key,long count=1){long previous;values.TryGetValue(key,out previous);values[key]=checked(previous+count);}
        public static Dictionary<string,object> Counts(Dictionary<string,long> values){return values.ToDictionary(entry=>entry.Key,entry=>(object)entry.Value);}
    }
}
