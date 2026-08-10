//! The smallest complete host: load a program, feed it, read what it says.
//!
//! Run with `cargo run --example echo`.
//!
//! Short on purpose. The point of the ABI is that a host loop is this size --
//! there is no scheduler to configure and no runtime to initialise, because
//! the program tells you what it is waiting for and you answer.

use diluvium::{Instance, Step};

const PROGRAM: &str = r#"
    local inbox = queue.lookup("inbox")
    local outbox = queue.lookup("outbox")
    while true do
        local _, msg = queue.wait({inbox})
        if msg == nil then return end
        queue.push(outbox, msg:upper())
    end
"#;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut inst = Instance::from_source(PROGRAM, "echo")?;
    let inbox = inst.queue("inbox").expect("every instance has an inbox");
    let outbox = inst.queue("outbox").expect("and an outbox");

    let mut step = inst.run()?;
    for line in ["hello", "from", "rust"] {
        // Parked, waiting on its inbox. Give it something and say so.
        assert!(matches!(step, Step::Parked(_)));
        inst.push(inbox, &line)?;
        step = inst.resume(inbox)?;
        if let Some(reply) = inst.pop::<String>(outbox)? {
            println!("{line} -> {reply}");
        }
    }
    Ok(())
}
