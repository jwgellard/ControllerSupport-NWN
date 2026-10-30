// Dump all functions (name, ns, addr, size) and vftable/typeinfo-ish symbols to file. Arg0 = outfile
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.PrintWriter;
public class DumpSyms extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    try (PrintWriter o = new PrintWriter(a[0], "UTF-8")) {
      for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(false)) {
        if (s.getSource() == SourceType.DEFAULT) continue;
        String t = s.getSymbolType().toString();
        if (t.equals("Label") || t.equals("Function") || t.equals("Class") ) {
          o.println(s.getAddress() + "\t" + t + "\t" + s.getName(true));
        }
      }
    }
  }
}
