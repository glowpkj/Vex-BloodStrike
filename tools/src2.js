const fs=require("fs");
const path="C:\\Program Files (x86)\\Steam\\steamapps\\common\\BLOODSTRIKE\\Engine\\Binaries\\Win64\\";
// Source 2 / tier0 / vstdlib markers
const probes={
 "tier0_s64.dll":["Source 2","VALVE","Valve","tier0","IClientEntity","CEntityInstance","CClientEntityList","CGameSceneNode","cliententity","Source2"],
 "vstdlib_s64.dll":["VALVE","Valve","vstdlib","Source 2"],
 "BloodStrike.exe":["CEntityInstance","CClientEntityList","IClientEntity","CGameSceneNode","CCSPlayerController","cliententity","CBaseEntity","Source 2"]
};
for(const [f,marks] of Object.entries(probes)){
  const p=path+f;
  if(!fs.existsSync(p)){console.log(f+" -> ausente");continue;}
  const b=fs.readFileSync(p);
  console.log("=== "+f+"  "+(b.length/1048576).toFixed(1)+" MB ===");
  for(const m of marks){
    let i=b.indexOf(Buffer.from(m,"latin1"));
    let n=0;
    while(i!==-1&&n<3){console.log("   "+m.padEnd(22)+" @ 0x"+i.toString(16));i=b.indexOf(Buffer.from(m,"latin1"),i+1);n++;}
    if(n===0)console.log("   "+m.padEnd(22)+" -");
  }
  console.log("");
}
