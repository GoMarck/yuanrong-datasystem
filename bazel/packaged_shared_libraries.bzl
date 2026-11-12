"""Copy shared libraries into package paths, stripping only the package copies."""

def _packaged_shared_libraries_impl(ctx):
    if ctx.attr.out and len(ctx.files.srcs) != 1:
        fail("out requires exactly one input library")

    outputs = []
    for src in ctx.files.srcs:
        out = ctx.actions.declare_file(ctx.attr.out or ctx.attr.out_dir + "/" + src.basename)
        ctx.actions.run_shell(
            inputs = [src],
            outputs = [out],
            arguments = [src.path, out.path],
            command = 'objcopy --strip-all "$1" "$2"' if ctx.attr.strip_symbols else 'cp -L "$1" "$2"',
            mnemonic = "PackageSharedLibrary",
            progress_message = "Packaging shared library %s" % out.short_path,
        )
        outputs.append(out)

    return [DefaultInfo(files = depset(outputs), runfiles = ctx.runfiles(files = outputs))]

packaged_shared_libraries = rule(
    implementation = _packaged_shared_libraries_impl,
    attrs = {
        "srcs": attr.label_list(allow_files = True, mandatory = True),
        "out": attr.string(),
        "out_dir": attr.string(default = "yr/datasystem/lib"),
        "strip_symbols": attr.bool(default = True),
    },
)
