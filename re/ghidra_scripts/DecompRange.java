// Decompiles every function whose entry lies in [start, end).
// Args: <out.c> <start hex> <end hex>
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import java.io.PrintWriter;

public class DecompRange extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    DecompInterface d = new DecompInterface();
    d.openProgram(currentProgram);
    try (PrintWriter o = new PrintWriter(a[0], "UTF-8")) {
      Function f = getFunctionAt(toAddr(a[1]));
      if (f == null) f = getFunctionAfter(toAddr(a[1]));
      long end = Long.parseLong(a[2], 16);
      for (; f != null && f.getEntryPoint().getOffset() < end; f = getFunctionAfter(f.getEntryPoint())) {
        o.println("// ===== " + f.getEntryPoint() + " " + f.getName(true) + " size=" + f.getBody().getNumAddresses());
        DecompileResults r = d.decompileFunction(f, 60, monitor);
        o.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed");
      }
    }
  }
}
