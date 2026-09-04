//! A sandboxed host, with a fuel budget and a relay between two instances.
//!
//! Run with: cargo run -p diluvium-wasmtime --example sandboxed -- path/to/diluvium_wasi.wasm
//!
//! Short on purpose, like the native example — but note what this one gets that
//! the native one cannot: the programs run inside a wasm sandbox, and the budget
//! on the second one is enforced from outside it.

use diluvium_wasmtime::{Endpoints, Runtime, Step};

fn main() -> anyhow::Result<()> {
    let path = std::env::args()
        .nth(1)
        .ok_or_else(|| anyhow::anyhow!("usage: sandboxed <diluvium.wasm>"))?;
    let wasm = std::fs::read(&path)?;
    let rt = Runtime::new(&wasm)?;

    // -- a program that echoes, driven by a host that answers ----------------
    let mut echo = rt.load(
        r#"
        local inbox = queue.lookup("inbox")
        local outbox = queue.lookup("outbox")
        while true do
            local _, msg = queue.wait({inbox})
            if msg == nil then return end
            queue.push(outbox, msg:upper())
        end
        "#,
        "echo",
    )?;
    let inbox = echo.queue("inbox")?.unwrap();
    let outbox = echo.queue("outbox")?.unwrap();

    let mut step = echo.run()?;
    for line in ["hello", "from", "wasmtime"] {
        assert!(matches!(step, Step::Parked(_)));
        echo.push(inbox, &line)?;
        step = echo.resume(inbox)?;
        if let Some(reply) = echo.pop::<String>(outbox)? {
            println!("{line} -> {reply}");
        }
    }

    // -- a runaway program, stopped from outside itself ----------------------
    //
    // This is the reason to host under wasmtime. §9.4's instruction budget uses
    // a Lua hook, which runs inside the thing it limits; fuel does not, so a
    // program cannot outlast it or spin somewhere a hook never fires.
    let mut runaway = rt.load("while true do end", "runaway")?;
    runaway.set_fuel(2_000_000)?;
    match runaway.run() {
        Err(e) => println!("runaway stopped from outside: {e}"),
        Ok(_) => println!("runaway finished, which it should not have"),
    }

    // -- two instances, and a host that only moves bytes ---------------------
    let mut a = rt.load(
        r#"
        local ref = select(2, queue.wait({queue.lookup("inbox")}))
        local up = endpoint.bind(ref, "upstream")
        for i = 1, 3 do queue.push(up, { n = i }) end
        "#,
        "sender",
    )?;
    let mut b = rt.load(
        r#"
        local inbox = queue.lookup("inbox")
        local out = queue.declare("final", {capacity = 8, exported = true})
        for _ = 1, 3 do
            local _, m = queue.wait({inbox})
            queue.push(out, m.n * 10)
        end
        "#,
        "receiver",
    )?;

    let mut book = Endpoints::default();
    let (token, ref_bytes) = book.mint("receiver");
    // The host chooses what a reference means; the guest never reads it. Told up
    // front rather than answered on demand, because a wasm host cannot install a
    // C callback -- see Instance::allow_endpoint.
    a.allow_endpoint(&ref_bytes, token)?;

    let a_inbox = a.queue("inbox")?.unwrap();
    a.run()?;
    a.push_raw(a_inbox, &Endpoints::as_message(&ref_bytes))?;
    a.resume(a_inbox)?;
    b.run()?;

    let ep = a.endpoint_queue(token)?.expect("the sender bound it");
    let b_inbox = b.queue("inbox")?.unwrap();
    let moved = diluvium_wasmtime::relay(&mut a, ep, &mut b, b_inbox)?;
    println!("relayed {moved} messages between two sandboxes");

    let final_q = b.queue("final")?.unwrap();
    while let Some(v) = b.pop::<u32>(final_q)? {
        println!("receiver produced {v}");
    }
    Ok(())
}
