use wasmtime::*;
use wasmtime_wasi::preview1::{add_to_linker_sync, WasiP1Ctx};
use wasmtime_wasi::p2::WasiCtxBuilder;
use std::env;

fn main() -> Result<()> {
    let args = env::args().collect::<Vec<_>>();

    if args.len() < 1 {
        eprintln!("Usage: {} <path-to-wasm>", args[0]);
        std::process::exit(1);
    }

    let wasm_path = &args[1];

    println!("Running WASM file: {}", wasm_path);

    let engine = Engine::default();
    let mut linker = Linker::new(&engine);
    add_to_linker_sync(&mut linker, |s| s)?;
    //add_to_linker_sync(&mut linker)?;

    let wasi = WasiCtxBuilder::new().inherit_stdio().inherit_args().inherit_env().build_p1();

    let ty = GlobalType::new(ValType::I32, Mutability::Const);

    let mut store = Store::new(&engine, wasi);
    let global = Global::new(&mut store, ty, Val::I32(2222))?;
    linker.define(&store, "env", "__waslr_seed", global)?;
    let debug_func = Func::wrap(&mut store, |n: i32| {
        println!("debug_early called with: {}", n);
    });
    linker.define(&store, "env", "debug_early", debug_func)?;

    let console_uintptr = Func::wrap(
        &mut store,
        move |mut caller: Caller<'_, WasiP1Ctx>, varname_ptr: i32, arg: i32| {
            // Get the module's linear memory
            let memory = match caller.get_export("memory") {
                Some(Extern::Memory(mem)) => mem,
                _ => {
                    eprintln!("No memory found in module");
                    return;
                }
            };

            // Read bytes starting at varname_ptr until null terminator
            let mut buf = Vec::new();
            let mut offset = varname_ptr as usize;
            loop {
                let mut byte = [0u8];
                if memory.read(&caller, offset, &mut byte).is_err() {
                    eprintln!("Failed to read memory at offset {}", offset);
                    return;
                }
                if byte[0] == 0 {
                    break;
                }
                buf.push(byte[0]);
                offset += 1;
            }

            let name = String::from_utf8_lossy(&buf);
            println!("{} --> {}", name, arg);
        },
    );

    linker.define(&store, "env", "console_uintptr", console_uintptr)?;

    let module = Module::from_file(&engine, wasm_path)?;

    linker.module(&mut store, "", &module)?;
    linker
        .get_default(&mut store, "")?
        .typed::<(), ()>(&store)?
        .call(&mut store, ())?;

    Ok(())
}
