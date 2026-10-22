// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace Ifss.Native {
    public static class ProjectBuild {
        static readonly string[] Inputs={"CMakeLists.txt","inFAMOUS Second Son.ico","build.bat",
            "scripts/build.ps1","scripts/build_tools.ps1","scripts/tools.ps1",
            "scripts/native/ProjectBuild.cs","scripts/native/NativeCommand.cs","scripts/native_tools.ps1"};
        static bool Configuration(string value) {return value=="RelWithDebInfo" || value=="Release" || value=="Debug";}
        static void Collect(ProjectPaths paths,string relative,List<string> files) {
            string directory=paths.Child(relative);
            foreach(string file in Directory.EnumerateFiles(directory)) files.Add(paths.Relative(paths.Child(paths.Relative(file))));
            foreach(string child in Directory.EnumerateDirectories(directory)) Collect(paths,paths.Relative(child),files);
        }
        static void Feed(HashAlgorithm hash,byte[] data) {hash.TransformBlock(data,0,data.Length,data,0);}
        public static Dictionary<string,object> Snapshot(ProjectPaths paths) {
            var files=new List<string>(Inputs);Collect(paths,"src",files);Collect(paths,"cmake",files);files.Sort(StringComparer.Ordinal);
            var manifest=Json.Read(paths.Child("project.json"));
            using(var hash=SHA256.Create()) {
                foreach(string file in files) {
                    Feed(hash,Encoding.UTF8.GetBytes(file+"\0"));
                    Feed(hash,ProjectPaths.Unhex(ProjectPaths.HashFile(paths.Child(file))));
                }
                foreach(string key in new[]{"dependencies_root","dependency_build_root"})
                    Feed(hash,Encoding.UTF8.GetBytes(key+"\0"+Json.Text(manifest,key,"")+"\0"));
                hash.TransformFinalBlock(new byte[0],0,0);
                return new Dictionary<string,object>{{"source_sha256",ProjectPaths.Hex(hash.Hash)},{"source_files",files.Count}};
            }
        }
        static void CheckImage(string file) {
            using(var stream=File.OpenRead(file)) {
                var header=new byte[64];if(stream.Read(header,0,header.Length)!=header.Length || header[0]!='M' || header[1]!='Z')
                    throw new InvalidDataException("Build output is not a Windows executable.");
                long offset=GameImage.U32(header,60);
                if(offset>stream.Length-6) throw new InvalidDataException("Build output is not a Windows x64 executable.");
                stream.Position=offset;var pe=new byte[6];
                if(stream.Read(pe,0,pe.Length)!=pe.Length || pe[0]!='P' || pe[1]!='E' || pe[2]!=0 || pe[3]!=0 || pe[4]!=0x64 || pe[5]!=0x86)
                    throw new InvalidDataException("Build output is not a Windows x64 executable.");
            }
        }
        public static Dictionary<string,object> Activate(ProjectPaths paths,string configuration,string expectedSource) {
            if(!Configuration(configuration)) throw new InvalidDataException("Unsupported build configuration.");
            var snapshot=Snapshot(paths);
            if(Json.Text(snapshot,"source_sha256","")!=expectedSource)
                throw new InvalidDataException("Project source changed during the build. Run build.bat again; runtime was not switched.");
            string relative="Build/"+configuration+"/inFAMOUS Second Son.exe",executable=paths.Child(relative);
            CheckImage(executable);
            string pdbRelative="Build/"+configuration+"/inFAMOUS Second Son.pdb",pdb=paths.Child(pdbRelative);
            bool symbols=File.Exists(pdb);
            if(configuration!="Release" && (!symbols || new FileInfo(pdb).Length==0))
                throw new InvalidDataException("The requested debug symbols were not generated; runtime was not switched.");
            string receiptRelative="Build/"+configuration+"/build-receipt.json";
            var project=Json.Read(paths.Child("project.json"));
            var receipt=new Dictionary<string,object>{{"schema",1},{"runtime",relative},{"runtime_sha256",ProjectPaths.HashFile(executable)},
                {"configuration",configuration},{"source_sha256",snapshot["source_sha256"]},{"source_files",snapshot["source_files"]},
                {"pdb",symbols?pdbRelative:null},{"pdb_sha256",symbols?ProjectPaths.HashFile(pdb):null},
                {"built_at_utc",DateTime.UtcNow.ToString("o")},{"external_dependency_sources",project["dependencies_root"]},
                {"external_dependency_libraries",project["dependency_build_root"]},{"game_started",false},{"game_acceptance_verified",false}};
            project["phase"]="project-engine";project["runtime_path"]=relative;project["runtime_sha256"]=receipt["runtime_sha256"];
            project["runtime_source_equivalence"]="project-source-snapshot";project["runtime_revision"]="project-snapshot:"+snapshot["source_sha256"];
            project["runtime_build_receipt"]=receiptRelative;project["new_executable_built"]=true;project["game_acceptance_verified"]=false;
            paths.WriteBuildReceipt(configuration,receipt);paths.WriteManifest(project);
            return receipt;
        }
        public static Dictionary<string,object> Verify(ProjectPaths paths,Dictionary<string,object> project,string executable) {
            string relative=Json.Text(project,"runtime_path",""),expected=paths.Child(relative);
            if(!String.Equals(executable,expected,StringComparison.OrdinalIgnoreCase) || !ProjectPaths.Inside(paths.Child("Build"),expected))
                throw new InvalidDataException("Project engine must be inside this project Build directory.");
            if(Json.Text(project,"phase","")!="project-engine" || !Json.Bool(project,"new_executable_built",false))
                throw new InvalidDataException("Project engine has not been built. Run build.bat first; external engines are not used.");
            if(!File.Exists(expected)) throw new FileNotFoundException("Project engine is missing. Run build.bat to restore it.");
            string receiptPath=Json.Text(project,"runtime_build_receipt","");var receipt=Json.Read(paths.Child(receiptPath));
            string configuration=Json.Text(receipt,"configuration","");
            if(!Configuration(configuration) || relative!="Build/"+configuration+"/inFAMOUS Second Son.exe" || receiptPath!="Build/"+configuration+"/build-receipt.json")
                throw new InvalidDataException("Invalid project build output or receipt path.");
            if(Json.Integer(Json.Get(receipt,"schema",0))!=1 || Json.Text(receipt,"runtime","")!=relative ||
                Json.Text(receipt,"runtime_sha256","")!=Json.Text(project,"runtime_sha256",""))
                throw new InvalidDataException("Build receipt does not match the selected engine.");
            string source=Json.Text(receipt,"source_sha256","");
            if(Json.Text(Snapshot(paths),"source_sha256","")!=source)
                throw new InvalidDataException("Project C++ or build inputs changed. Run build.bat before starting the game.");
            return new Dictionary<string,object>{{"kind","project-build"},{"source_checked",true},
                {"source_sha256",source},{"configuration",configuration},{"receipt",receiptPath}};
        }
    }
}
