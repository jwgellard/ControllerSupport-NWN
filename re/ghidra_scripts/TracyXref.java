// args: outfile substr... ; for strings matching, find tracy srcloc struct refs then code functions
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;
import java.io.PrintWriter;
public class TracyXref extends GhidraScript {
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
          Address from = r.getFromAddress();
          Function f = getFunctionContaining(from);
          if (f != null) { o.println("   code " + from + " in " + f.getEntryPoint() + " " + f.getName(true)); continue; }
          for (int off = 0; off >= -24; off -= 8) {
            Address s = from.add(off);
            for (Reference r2 : getReferencesTo(s)) {
              Function f2 = getFunctionContaining(r2.getFromAddress());
              if (f2 != null) o.println("   srcloc " + s + " <- " + r2.getFromAddress() + " in " + f2.getEntryPoint() + " " + f2.getName(true));
            }
          }
        }
      }
    }
  }
}
