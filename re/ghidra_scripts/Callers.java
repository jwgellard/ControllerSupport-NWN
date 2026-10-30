// args: outfile addr... ; lists callers (function entry + call site) of each function
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
public class Callers extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    try (PrintWriter o = new PrintWriter(a[0], "UTF-8")) {
      for (int i = 1; i < a.length; i++) {
        o.println("TARGET " + a[i]);
        for (Reference r : getReferencesTo(toAddr(a[i]))) {
          Function f = getFunctionContaining(r.getFromAddress());
          o.println("   " + r.getReferenceType() + " " + r.getFromAddress() + " in " + (f == null ? "?" : f.getEntryPoint() + " " + f.getName(true)));
        }
      }
    }
  }
}
