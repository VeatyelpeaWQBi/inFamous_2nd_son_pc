﻿// SPDX-License-Identifier: GPL-2.0-or-later
// Offline regression only: synthetic binaries/configs/CSV; no game, UI, profiler or external engine.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml.Linq;

namespace Ifss.Native {
    public static class BootstrapTests {
        sealed class Zeros : IGameImage { public byte[] Read(ulong address,int size) { return new byte[size]; } }
        static void Require(bool condition,string message) { if (!condition) throw new InvalidOperationException(message); }
        static void Reject<T>(Action action) where T:Exception {
            try { action(); } catch (T) { return; }
            throw new InvalidOperationException("Expected rejection: " + typeof(T).Name);
        }
        static XElement Xml() { return XElement.Parse("<Patch><TitleID><ID>CUSA00309</ID></TitleID></Patch>"); }
        static XElement Patch(XElement root,string name,string value) {
            var meta=new XElement("Metadata",new XAttribute("Name",name),new XAttribute("isEnabled","true"),new XAttribute("AppVer","01.07"),new XAttribute("AppElf","eboot.bin"),
                new XElement("PatchList",new XElement("Line",new XAttribute("Type","bytes"),new XAttribute("Address","0x400010"),new XAttribute("Value",value),new XAttribute("OriginalHash",GameImage.Fnv1a(new byte[1])))));
            root.Add(meta); return meta;
        }
        static Dictionary<string,object> Identity() { return new Dictionary<string,object>{{"TITLE_ID","CUSA00309"},{"APP_VER","01.07"}}; }
        static Dictionary<string,object> Preferences() {
            return new Dictionary<string,object>{{"schema",1},{"console_language",1},{"is_circle_enter",false},{"show_fps",true}};
        }
        static Dictionary<string,object> Confirm(bool value) {
            return new Dictionary<string,object>{{"scene",value},{"brightness",value},{"particles",value},{"resolution",value}};
        }
        static byte[] Elf() {
            using(var memory=new MemoryStream()) {
                using(var writer=new BinaryWriter(memory,Encoding.UTF8,true)) {
                    writer.Write(new byte[]{0x7f,0x45,0x4c,0x46,2,1,1}); writer.Write(new byte[9]);
                    writer.Write((ushort)2);writer.Write((ushort)62);writer.Write((uint)1);writer.Write((ulong)0);writer.Write((ulong)64);writer.Write((ulong)0);writer.Write((uint)0);
                    foreach(ushort value in new ushort[]{64,56,1,0,0,0})writer.Write(value);
                    writer.Write((uint)1);writer.Write((uint)5);writer.Write((ulong)120);writer.Write((ulong)0);writer.Write((ulong)0);
                    writer.Write((ulong)1);writer.Write((ulong)2);writer.Write((ulong)16);writer.Write((byte)0xab);
                }
                return memory.ToArray();
            }
        }
        static byte[] Sfo() {
            byte[] keys=Encoding.UTF8.GetBytes("TITLE_ID\0APP_VER\0"),title=Encoding.UTF8.GetBytes("CUSA00309\0"),version=Encoding.UTF8.GetBytes("01.07\0");
            using(var stream=new MemoryStream()) {
                using(var writer=new BinaryWriter(stream,Encoding.UTF8,true)) {
                    writer.Write((uint)0x46535000);writer.Write((uint)0x101);writer.Write((uint)52);writer.Write((uint)(52+keys.Length));writer.Write((uint)2);
                    writer.Write((ushort)0);writer.Write((ushort)0x204);writer.Write((uint)title.Length);writer.Write((uint)title.Length);writer.Write((uint)0);
                    writer.Write((ushort)9);writer.Write((ushort)0x204);writer.Write((uint)version.Length);writer.Write((uint)version.Length);writer.Write((uint)title.Length);
                    writer.Write(keys);writer.Write(title);writer.Write(version);
                }
                return stream.ToArray();
            }
        }
        public static void Run(string root) {
            int passed=0;var failures=new List<string>();
            Action<string,Action> check=delegate(string name,Action action) {
                try {action();++passed;} catch(Exception error){failures.Add(name+": "+error.GetBaseException().Message);}
            };
            check("conflicting patch bytes",delegate {
                var xml=Xml();Patch(xml,"one","01");Patch(xml,"two","02");Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,Identity(),"1080p"));
            });
            check("wrong patch version",delegate {
                var xml=Xml();Patch(xml,"one","01");var identity=Identity();identity["APP_VER"]="01.00";
                Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,identity,"1080p"));
            });
            check("wrong title",delegate {
                var xml=Xml();Patch(xml,"one","01");var identity=Identity();identity["TITLE_ID"]="CUSA00000";
                Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,identity,"1080p"));
            });
            check("missing original hash",delegate {
                var xml=Xml();Patch(xml,"one","01").Element("PatchList").Element("Line").Attribute("OriginalHash").Remove();
                Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,Identity(),"1080p"));
            });
            check("empty byte patch",delegate {
                var xml=Xml();Patch(xml,"one","");Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,Identity(),"1080p"));
            });
            check("patch below image base",delegate {
                var xml=Xml();Patch(xml,"one","01").Element("PatchList").Element("Line").SetAttributeValue("Address","0x3fffff");
                Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,Identity(),"1080p"));
            });
            check("six exclusive resolution patches",delegate {
                foreach(var resolution in LauncherSettings.Resolutions) {
                    var xml=Xml();Patch(xml,"capacity","01");foreach(var choice in LauncherSettings.Resolutions)if(choice.Patch!=null)Patch(xml,choice.Patch,"01");
                    Preflight.SelectResolution(xml,resolution.Key);var audited=Preflight.AuditXml(new Zeros(),xml,Identity(),resolution.Key);
                    Require(audited.Count==(resolution.Patch==null ? 1 : 2),"Wrong selected resolution count.");
                }
            });
            check("duplicate resolution",delegate {
                var xml=Xml();Patch(xml,LauncherSettings.Find("pro4k").Patch,"01");Patch(xml,LauncherSettings.Find("pro4k").Patch,"01");
                Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,Identity(),"pro4k"));
            });
            check("wrong resolution",delegate {
                var xml=Xml();Patch(xml,LauncherSettings.Find("1440p").Patch,"01");
                Reject<InvalidDataException>(()=>Preflight.AuditXml(new Zeros(),xml,Identity(),"pro4k"));
            });
            check("bounded ELF reader",delegate {
                var image=new GameImage(Elf());Require(image.Read(0,1)[0]==0xab,"Wrong load offset.");
                Require(image.Read(8,4).All(x=>x==0),"BSS/alignment was not zero-filled.");
                Reject<InvalidDataException>(()=>image.Read(15,2));Reject<InvalidDataException>(()=>image.Read(UInt64.MaxValue,2));
            });
            check("SFO identity and malformed offsets",delegate {
                var bytes=Sfo();var identity=GameImage.Identity(bytes);Require((string)identity["TITLE_ID"]=="CUSA00309" && (string)identity["APP_VER"]=="01.07","Wrong SFO identity.");
                Buffer.BlockCopy(BitConverter.GetBytes((uint)0),0,bytes,8,4);Reject<InvalidDataException>(()=>GameImage.Identity(bytes));
                Reject<InvalidDataException>(()=>GameImage.Identity(new byte[4]));
            });
            check("truncated ELF",delegate {Reject<InvalidDataException>(()=>new GameImage(new byte[8]));});
            check("invalid segment sizes",delegate {
                var bytes=Elf();Buffer.BlockCopy(BitConverter.GetBytes((ulong)2),0,bytes,96,8);Buffer.BlockCopy(BitConverter.GetBytes((ulong)1),0,bytes,104,8);
                Reject<InvalidDataException>(()=>new GameImage(bytes));
            });
            check("legacy settings",delegate {
                var settings=LauncherSettings.Validate(Preferences(),new[]{new Language("English",1,"english")});
                Require((string)settings["resolution"]=="1080p" && (string)settings["display_mode"]=="fullscreen" && (int)settings["schema"]==3,"Legacy defaults changed.");
            });
            check("unknown resolution",delegate {Reject<InvalidDataException>(()=>LauncherSettings.Find("3440x1440"));});
            check("strict setting types",delegate {
                var settings=Preferences();settings["show_fps"]=1;Reject<InvalidDataException>(()=>LauncherSettings.Validate(settings,new[]{new Language("English",1,"english")}));
                settings=Preferences();settings.Remove("show_fps");Reject<InvalidDataException>(()=>LauncherSettings.Validate(settings,new[]{new Language("English",1,"english")}));
            });
            check("display modes preserve rendering baseline",delegate {
                foreach(var display in new[]{"fullscreen","windowed","borderless"}) {
                    var config=new Dictionary<string,object>();var gpu=Json.Group(config,"GPU");gpu["readbacks_mode"]=2;gpu["hdr_allowed"]=false;
                    var settings=Preferences();settings["display_mode"]=display;LauncherSettings.Apply(config,settings);LauncherSettings.ApplyResolution(config,"pro4k");
                    Preflight.CheckConfig(config,LauncherSettings.Find("pro4k"));Require((bool)gpu["full_screen"]==(display!="windowed"),"Wrong fullscreen policy.");
                }
            });
            check("six rendering baselines",delegate {
                foreach(var resolution in LauncherSettings.Resolutions) {
                    var config=new Dictionary<string,object>();var gpu=Json.Group(config,"GPU");gpu["readbacks_mode"]=2;gpu["hdr_allowed"]=false;
                    LauncherSettings.ApplyResolution(config,resolution.Key);Preflight.CheckConfig(config,resolution);
                }
            });
            check("JSON root and boolean types",delegate {Reject<InvalidDataException>(()=>Json.Object(Json.Decode("[]")));Reject<InvalidDataException>(()=>Json.Integer(true));});
            check("frozen mode validation happens before file access",delegate {Reject<InvalidDataException>(()=>Preflight.Inspect(new ProjectPaths(root),"720p",null,null,true));});
            check("Windows argument quoting",delegate {Require(GameIdentity.Quote("x\"y")=="\"x\\\"y\"","Quoted argument changed.");});
            check("paths, atomic replacement and preference persistence",delegate {
                var project=new ProjectPaths(root);string temporary=project.Child("state/offline-tests/native-"+Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(temporary);
                try {
                    var paths=new ProjectPaths(temporary);Reject<InvalidDataException>(()=>paths.Child("../outside"));Reject<InvalidDataException>(()=>paths.Child("."));
                    Reject<InvalidDataException>(()=>paths.Resolve("C:relative"));
                    string target=paths.Child("state/value.json");paths.WriteJson(target,new Dictionary<string,object>{{"value","old"}});
                    using(var locked=new FileStream(target,FileMode.Open,FileAccess.Read,FileShare.Read)) {
                        Reject<IOException>(()=>paths.WriteJson(target,new Dictionary<string,object>{{"value","new"}}));
                        Require((string)Json.Read(target)["value"]=="old","Old config was lost after denied replacement.");
                    }
                    paths.WriteJson(target,new Dictionary<string,object>{{"value","new"}});Require((string)Json.Read(target)["value"]=="new","Atomic replacement failed.");
                    Directory.CreateDirectory(paths.Child("game/CUSA00309/art/cache"));File.WriteAllText(paths.Child("game/CUSA00309/art/cache/lang_english_text.xpps"),"");
                    File.WriteAllText(Path.Combine(temporary,"project.json"),"{\"game_relative_path\":\"game/CUSA00309\"}");
                    var settings=Preferences();settings["resolution"]="pro4k";settings["display_mode"]="borderless";LauncherSettings.Save(paths,settings);
                    var overridden=LauncherSettings.Load(paths,"1080p");Require((string)overridden["resolution"]=="1080p","Mode override failed.");
                    Require((string)Json.Read(paths.Child("state/play/launcher.json"))["resolution"]=="pro4k","Transient override changed saved preference.");
                    Reject<InvalidDataException>(()=>paths.WriteText(paths.Child("game/eboot.bin"),"forbidden"));
                    // The shared release menu must not require main's manifest, baseline or fixed game subdirectory.
                    var portable=new ProjectPaths(Path.Combine(temporary,"portable"));string game=Path.Combine(portable.Root,"game");
                    Directory.CreateDirectory(Path.Combine(game,"art","cache"));File.WriteAllText(Path.Combine(game,"art","cache","lang_english_text.xpps"),"");
                    var portableSettings=LauncherSettings.Load(portable,"pro4k",game);
                    Require((string)portableSettings["resolution"]=="pro4k" && Json.Integer(portableSettings["console_language"])==1,"Release menu defaults require development files.");
                    LauncherSettings.Save(portable,portableSettings,game);
                    Require((string)LauncherSettings.Load(portable,"auto",game)["resolution"]=="pro4k","Release preferences were not retained.");
                } finally {
                    if(!ProjectPaths.Inside(project.Child("state/offline-tests"),temporary))throw new InvalidOperationException("Unsafe regression cleanup path.");
                    if(Directory.EnumerateFileSystemEntries(temporary,"*",SearchOption.AllDirectories).Any(p=>(File.GetAttributes(p)&FileAttributes.ReparsePoint)!=0))
                        throw new InvalidDataException("Regression cleanup refuses links; fixtures retained.");
                    Directory.Delete(temporary,true);
                }
            });
            if(failures.Count!=0)throw new InvalidOperationException(String.Join("\n",failures));
            Console.WriteLine("Native bootstrap regressions passed: "+passed+" cases. No real game or user save was opened.");
        }
    }
}
