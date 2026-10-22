// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
namespace Ifss.Native {
    public static class NativeBuildTests {
        static void Require(bool condition,string message){if(!condition)throw new InvalidOperationException(message);}
        static void Reject(Action action,string reason){try{action();}catch(Exception error){if(error.Message.IndexOf(reason,StringComparison.OrdinalIgnoreCase)>=0)return;throw;}throw new InvalidOperationException("Expected rejection: "+reason);}
        public static void Run(string project) {
            var owner=new ProjectPaths(project);string directory=owner.Child("state/offline-tests/build-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(directory);
            try {
                var paths=new ProjectPaths(directory);
                foreach(string name in new[]{"CMakeLists.txt","inFAMOUS Second Son.ico","build.bat","dependencies.lock.json","scripts/build.ps1","scripts/build_tools.ps1","scripts/tools.ps1","scripts/native/ProjectBuild.cs","scripts/native/NativeCommand.cs","scripts/native_tools.ps1","src/main.cpp","cmake/test.cmake"}){
                    string file=paths.Child(name);Directory.CreateDirectory(Path.GetDirectoryName(file));File.WriteAllText(file,"fixture",Encoding.UTF8);
                }
                paths.WriteManifest(new Dictionary<string,object>{{"phase","project-engine-pending"},{"runtime_path","Build/RelWithDebInfo/inFAMOUS Second Son.exe"},{"new_executable_built",false},{"dependencies_root","externals"},{"dependency_build_root","externals/prebuilt"}});
                string image=paths.Child("Build/RelWithDebInfo/inFAMOUS Second Son.exe"),pdb=paths.Child("Build/RelWithDebInfo/inFAMOUS Second Son.pdb");Directory.CreateDirectory(Path.GetDirectoryName(image));
                var bytes=new byte[70];bytes[0]=(byte)'M';bytes[1]=(byte)'Z';bytes[60]=64;bytes[64]=(byte)'P';bytes[65]=(byte)'E';bytes[68]=0x64;bytes[69]=0x86;File.WriteAllBytes(image,bytes);File.WriteAllText(pdb,"symbol fixture");
                Reject(()=>ProjectBuild.Verify(paths,Json.Read(paths.Child("project.json")),image),"has not been built");
                var localManifest=Json.Read(paths.Child("project.json"));
                foreach(string key in new[]{"dependencies_root","dependency_build_root"}) {
                    foreach(string external in new[]{"../old-project/externals","C:/external/dependencies"}) {
                        var invalid=new Dictionary<string,object>(localManifest);invalid[key]=external;paths.WriteManifest(invalid);
                        Reject(()=>ProjectBuild.Snapshot(paths),"Project dependency paths");
                    }
                }
                paths.WriteManifest(localManifest);
                string source=Json.Text(ProjectBuild.Snapshot(paths),"source_sha256","");
                ProjectBuild.Activate(paths,"RelWithDebInfo",source);var manifest=Json.Read(paths.Child("project.json"));
                Require(Json.Bool(ProjectBuild.Verify(paths,manifest,image),"source_checked",false),"Activated source was not verified.");
                Require(!Json.Bool(manifest,"game_acceptance_verified",true),"A build falsely claimed game acceptance.");
                byte[] before=File.ReadAllBytes(paths.Child("project.json"));
                string input=paths.Child("src/main.cpp");File.WriteAllText(input,"changed source");
                Reject(()=>ProjectBuild.Verify(paths,manifest,image),"changed");Reject(()=>ProjectBuild.Activate(paths,"RelWithDebInfo",source),"changed during");
                Require(before.SequenceEqual(File.ReadAllBytes(paths.Child("project.json"))),"Failed activation changed the runtime selection.");
                source=Json.Text(ProjectBuild.Snapshot(paths),"source_sha256","");File.WriteAllText(image,"not an executable");
                Reject(()=>ProjectBuild.Activate(paths,"RelWithDebInfo",source),"Windows executable");
                File.WriteAllBytes(image,bytes);File.Delete(pdb);Reject(()=>ProjectBuild.Activate(paths,"RelWithDebInfo",source),"debug symbols");
                manifest["runtime_path"]="../outside.exe";Reject(()=>ProjectBuild.Verify(paths,manifest,image),"escaped");
                Require(before.SequenceEqual(File.ReadAllBytes(paths.Child("project.json"))),"Rejected image/symbol inputs changed the runtime selection.");
                Console.WriteLine("Native build provenance regressions passed: pending/source/image/symbol/path/atomic selection guards. No real executable was built or run.");
            } finally {
                if(!ProjectPaths.Inside(owner.Child("state/offline-tests"),directory))throw new InvalidDataException("Unsafe fixture cleanup path.");
                if(Directory.EnumerateFileSystemEntries(directory,"*",SearchOption.AllDirectories).Any(p=>(File.GetAttributes(p)&FileAttributes.ReparsePoint)!=0))throw new InvalidDataException("Fixture cleanup refuses links.");
                Directory.Delete(directory,true);
            }
        }
    }
}
