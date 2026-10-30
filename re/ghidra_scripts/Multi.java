// args: cmdfile outfile. cmd lines: "S substr" string xrefs, "C addr" callers, "D addr|name" decompile, "N regex" function names matching
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.util.DefinedStringIterator;
import java.io.PrintWriter;
import java.nio.file.*;
public class Multi extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    DecompInterface ifc = new DecompInterface(); ifc.openProgram(currentProgram);
    try (PrintWriter o = new PrintWriter(a[1], "UTF-8")) {
      for (String line : Files.readAllLines(Paths.get(a[0]))) {
        line = line.trim(); if (line.length() < 3) continue;
        char c = line.charAt(0); String arg = line.substring(2).trim();
        o.println("##### " + line);
        if (c == 'S') {
          for (Data d : DefinedStringIterator.forProgram(currentProgram)) {
            String v = d.getDefaultValueRepresentation();
            if (!v.contains(arg)) continue;
            o.println("STR " + d.getAddress() + " " + v);
            for (Reference r : getReferencesTo(d.getAddress())) {
              Function f = getFunctionContaining(r.getFromAddress());
              o.println("   ref " + r.getFromAddress() + " in " + (f == null ? "?" : f.getEntryPoint() + " " + f.getName(true)));
            }
          }
        } else if (c == 'C') {
          for (Reference r : getReferencesTo(toAddr(arg))) {
            Function f = getFunctionContaining(r.getFromAddress());
            o.println("   " + r.getReferenceType() + " " + r.getFromAddress() + " in " + (f == null ? "?" : f.getEntryPoint() + " " + f.getName(true)));
          }
        } else if (c == 'D') {
          Function f = null;
          if (arg.matches("(0x)?[0-9a-fA-F]{8,}")) f = getFunctionContaining(toAddr(arg.replace("0x","")));
          else for (Symbol s : currentProgram.getSymbolTable().getSymbols(arg)) { f = getFunctionAt(s.getAddress()); if (f != null) break; }
          if (f == null) { o.println("// not found"); continue; }
          DecompileResults r = ifc.decompileFunction(f, 120, monitor);
          o.println("// " + f.getName(true) + " @ " + f.getEntryPoint());
          o.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed");
        } else if (c == 'N') {
          for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            if (f.getName(true).matches(arg)) o.println("   " + f.getEntryPoint() + " " + f.getName(true));
        }
        o.flush();
      }
    }
  }
}
