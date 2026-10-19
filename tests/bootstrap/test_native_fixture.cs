// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.IO;
using System.Linq;
using System.Text;
namespace Ifss.Native {
    public sealed class NativeTestFixture:IDisposable {
        readonly ProjectPaths owner;public readonly ProjectPaths Paths;public string Root{get{return Paths.Root;}}
        public NativeTestFixture(string root,string name){owner=new ProjectPaths(root);string folder=owner.Child("state/offline-tests/"+name+"-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(folder);Paths=new ProjectPaths(folder);}
        public string Write(string relative,string text){string file=Paths.Child(relative);Directory.CreateDirectory(Path.GetDirectoryName(file));File.WriteAllText(file,text,new UTF8Encoding(false));return file;}
        public void Json(string relative,object value){Write(relative,Ifss.Native.Json.Encode(value));}
        public void Bytes(string relative,byte[] bytes){string file=Paths.Child(relative);Directory.CreateDirectory(Path.GetDirectoryName(file));File.WriteAllBytes(file,bytes);}
        public void Dispose(){if(!ProjectPaths.Inside(owner.Child("state/offline-tests"),Root))throw new InvalidDataException("Unsafe fixture cleanup path.");if(Directory.EnumerateFileSystemEntries(Root,"*",SearchOption.AllDirectories).Any(path=>(File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0))throw new InvalidDataException("Fixture cleanup refuses links.");Directory.Delete(Root,true);}
    }
}
