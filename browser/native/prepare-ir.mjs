// Adapt March 0.7's browser IR for the one-chunk fixture.
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync } from 'node:fs';

const [input, declarations, output] = process.argv.slice(2);
let ir = readFileSync(input, 'utf8');
assert.match(ir, /^target triple = "wasm32-unknown-unknown"$/m);

// Replace the pointer-only declarations with the types used at the calls.
for (const name of ['sn_sample2', 'strata_browser_publish']) {
  const declaration = new RegExp(`^declare [^\\n@]+@${name}\\([^\\n]*\\)\\n`, 'm');
  assert.match(ir, declaration);
  ir = ir.replace(declaration, '');
}

const entry = '  call void @march_main()';
assert.ok(ir.includes(entry));
ir = ir.replace(entry, '  %result = call ptr @march_main(ptr @fixture_cap)');

// Decode signed tagged immediates at pointer width before widening.
let decoded = 0;
ir = ir.replace(/^define [^\n]+\n[\s\S]*?^}/gm, (fn) => {
  const pointerInts = new Set(Array.from(
    fn.matchAll(/^  (%\S+) = ptrtoint ptr .* to i64$/gm), (match) => match[1],
  ));
  return fn.replace(/^  (%\S+) = ashr(?: exact)? i64 (%\S+), 1$/gm, (line, dst, src) => {
    if (!pointerInts.has(src)) return line;
    decoded++;
    return `  ${dst}.narrow = trunc i64 ${src} to i32
  ${dst}.signed = ashr i32 ${dst}.narrow, 1
  ${dst} = sext i32 ${dst}.signed to i64`;
  });
});
assert.ok(decoded > 0);
writeFileSync(output, `${ir}\n${readFileSync(declarations, 'utf8')}`);
