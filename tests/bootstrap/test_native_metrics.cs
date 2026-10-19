// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
namespace Ifss.Native {
    public static class NativeMetricTests {
        const string Frames="frame,elapsed_ns,interval_ns\n0,0,0\n1,16000000,16000000\n2,32000000,16000000\n3,48000000,16000000\n";
        static Dictionary<string,object> Measure(string text,double start,double end,string mode,bool confirm){using(var reader=new StringReader(text))return Metrics.Summarize(reader,start,end,mode,DiagnosticData.Map("scene",confirm,"brightness",confirm,"particles",confirm,"resolution",confirm));}
        static void Check(bool condition,string error){if(!condition)throw new InvalidOperationException(error);}
        static void Reject(Action action){try{action();}catch(InvalidDataException){return;}throw new InvalidOperationException("Invalid measurement accepted.");}
        public static void Run(){
            var report=Measure(Frames,0,.048,"1080p",true);Check((string)report["baseline_acceptance"]=="candidate-pass"&&Math.Abs((double)report["fps"]-62.5)<1e-10,"Mean FPS or confirmation policy changed.");
            Check((string)Measure(Frames,0,.048,"1080p",false)["baseline_acceptance"]=="not-verified","Missing visual acceptance was ignored.");
            Reject(()=>Measure(Frames,0,Double.PositiveInfinity,"1080p",true));Reject(()=>Measure(Frames,0,.1,"1080p",true));
            Check((int)Measure(Frames,.01,.048,"1080p",true)["frames"]==2,"Partial frame included.");Check(Measure(Frames,0,.048,"1440p",true)["numeric_screen_pass"]==null,"Unsupported baseline invented.");
            Reject(()=>Measure(Frames.Replace("32000000,16000000","32000000,15000000"),0,.048,"1080p",true));Reject(()=>Measure(Frames.Replace("0,0,0\n",""),0,.048,"1080p",true));
            Check((int)Measure("\n"+Frames.Replace("\n","\n\n"),0,.048,"1080p",true)["frames"]==3,"Empty CSV lines changed measurement.");
            Check((int)Measure(Frames.Replace("frame,elapsed_ns,interval_ns","\"frame\",\"elapsed_ns\",\"interval_ns\""),0,.048,"1080p",true)["frames"]==3,"Quoted CSV failed.");
            var text=new StringBuilder("frame,elapsed_ns,interval_ns\n0,0,0\n");long elapsed=0;for(int i=1;i<=25;++i){elapsed+=i*1000000;text.Append(i+","+elapsed+","+(i*1000000)+"\n");}report=Measure(text.ToString(),0,.325,"1080p",true);Check((double)report["p95_ms"]==24&&(double)report["p99_ms"]==25,"Nearest rank percentiles changed.");
            var bytes=AnalyzeGame.ElfForWindow(0x100,new byte[]{0x90,0xc3});Check(GameImage.U32(bytes,0)==0x464c457f&&bytes[64]==0x90&&GameImage.U16(bytes,60)==3,"Static analysis ELF layout changed.");
            Console.WriteLine("Native metrics/static-analysis regressions passed: 12 checks. No real game was opened.");
        }
    }
}
