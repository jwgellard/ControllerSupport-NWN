// args: outfile substr... ; prints strings containing substr, and functions referencing them
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;
import java.io.PrintWriter;
public class StrXref extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    try (PrintWriter o = new PrintWriter(a[0], "UTF-8")) {
      for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
        String v = d.getDefaultValueRepresentation();
        boolean m = false;
        for (int i = 1; i < a.length; i++) if (v.contains(a[i])) m = true;
        if (!m) continue;
        o.println("STR " + d.getAddress() + " " + v);
        for (Reference r : getReferencesTo(d.getAddress())) {
          Function f = getFunctionContaining(r.getFromAddress());
          o.println("   ref " + r.getFromAddress() + " in " + (f == null ? "?" : f.getEntryPoint() + " " + f.getName(true)));
        }
      }
    }
  }
}
