import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;
public class FuncAround extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    try (PrintWriter o = new PrintWriter(a[0], "UTF-8")) {
      for (int i = 1; i < a.length; i++) {
        Function f = getFunctionContaining(toAddr(a[i]));
        o.println(a[i] + " -> " + (f==null?"none": f.getEntryPoint()+" "+f.getName(true)+" size="+f.getBody().getNumAddresses()));
        if (f != null && f.getName().startsWith("Catch")) {
          // find enclosing: scan back for non-catch function
          Function p = getFunctionBefore(f.getEntryPoint());
          while (p != null && p.getName().startsWith("Catch")) p = getFunctionBefore(p.getEntryPoint());
          o.println("   prev non-catch: " + (p==null?"none":p.getEntryPoint()+" "+p.getName(true)));
        }
      }
    }
  }
}
