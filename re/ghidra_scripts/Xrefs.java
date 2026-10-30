import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
import java.util.*;
public class Xrefs extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs();
    try(PrintWriter o=new PrintWriter(a[0],"UTF-8")){
      for(int i=1;i<a.length;i++){
        List<Address> targets=new ArrayList<>();
        if(a[i].matches("(0x)?[0-9a-fA-F]{8,}")) targets.add(toAddr(a[i].replace("0x","")));
        else for(Symbol s:currentProgram.getSymbolTable().getSymbols(a[i])) targets.add(s.getAddress());
        for(Address t:targets){
          o.println("## "+a[i]+" @ "+t);
          for(Reference r:getReferencesTo(t)){
            Function f=getFunctionContaining(r.getFromAddress());
            o.println("  "+r.getFromAddress()+" in "+(f==null?"?":f.getName()+"@"+f.getEntryPoint()));
          }
        }
      }
    }
  }
}
