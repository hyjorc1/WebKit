//@ requireOptions("--useWebAssemblySIMD=1")
//@ skip if !$isSIMDPlatform
import { instantiate } from "../wabt-wrapper.js"
import * as assert from "../assert.js"

let wat = `
    (module
        (memory $memory 1)
        (export "memory" (memory $memory))

        (func (export "load_first_item_in_mem") (param $offset i32) (result i64)
            local.get $offset
            i64.load32_u
        )
    )
`;

async function test() {
    const instance = await instantiate(wat, {}, { })
    const { memory, load_first_item_in_mem } = instance.exports;

    const dataView = new DataView(memory.buffer);
    dataView.setUint32(0, 1, true);     // memory: 01 00 00 00
    dataView.setUint32(4, 2, true);     // memory: 01 00 00 00 02 00 00 00
    // const offset = 65537; // 0x10001
    const offset = 65536; // 0x10000
    // const offset = 65533;
    // const offset = 65532;
    const result = load_first_item_in_mem(offset);
    print("result=" + result, "offset=" + offset, "");
    return result;
}

assert.asyncTest(test());
// assert.asyncTestEq(test(), 33554432n);

