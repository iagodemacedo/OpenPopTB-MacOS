// Exports every function's decompiled C plus strings and xrefs-to-strings,
// so the port can grep the whole binary without re-running Ghidra.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.data.StringDataInstance;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportAll extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outDir = getScriptArgs()[0];
        new File(outDir).mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        try (PrintWriter all = new PrintWriter(new FileWriter(outDir + "/decomp.c"));
             PrintWriter idx = new PrintWriter(new FileWriter(outDir + "/functions.tsv"))) {
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                idx.printf("%s\t%s\t%d\t%d%n", f.getEntryPoint(), f.getName(),
                        f.getBody().getNumAddresses(), f.getSymbol().getReferenceCount());
                DecompileResults r = ifc.decompileFunction(f, 120, monitor);
                all.printf("%n// ==== %s @ %s ====%n", f.getName(), f.getEntryPoint());
                if (r != null && r.decompileCompleted()) all.println(r.getDecompiledFunction().getC());
                else all.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
            }
        }
        try (PrintWriter s = new PrintWriter(new FileWriter(outDir + "/strings.tsv"))) {
            DataIterator di = currentProgram.getListing().getDefinedData(true);
            while (di.hasNext()) {
                Data d = di.next();
                if (!StringDataInstance.isString(d)) continue;
                StringBuilder refs = new StringBuilder();
                for (Reference ref : getReferencesTo(d.getAddress())) {
                    Function f = getFunctionContaining(ref.getFromAddress());
                    refs.append(f == null ? ref.getFromAddress().toString() : f.getName()).append(',');
                }
                String v = String.valueOf(d.getValue()).replace("\n", "\\n").replace("\t", "\\t");
                s.printf("%s\t%s\t%s%n", d.getAddress(), v, refs);
            }
        }
    }
}
