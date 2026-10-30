// Headless: decompile functions given as script args (hex addresses or symbol names) to an output file.
// Usage: -postScript Decomp.java <outfile> <addr|name> [...]
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import java.io.PrintWriter;

public class Decomp extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Function f = null;
                if (args[i].matches("(0x)?[0-9a-fA-F]{8,}")) {
                    Address a = toAddr(args[i].replace("0x", ""));
                    f = getFunctionContaining(a);
                } else {
                    for (Symbol s : currentProgram.getSymbolTable().getSymbols(args[i])) {
                        f = getFunctionAt(s.getAddress());
                        if (f != null) break;
                    }
                }
                if (f == null) { out.println("// not found: " + args[i]); continue; }
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                out.println("// ===== " + args[i] + " -> " + f.getName(true) + " @ " + f.getEntryPoint());
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
    }
}
