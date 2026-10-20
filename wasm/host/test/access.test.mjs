// node wasm/host/test/access.test.mjs : checks src/access.js with tokens signed by a throwaway key.
import assert from "node:assert/strict";
import { verifyAccessJwt } from "../src/access.js";

const team = "example-team.cloudflareaccess.com";
const aud = "aud-tag-of-the-app";
const { privateKey, publicKey } = await crypto.subtle.generateKey(
  { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]),
    hash: "SHA-256" }, true, ["sign", "verify"]);
const other = await crypto.subtle.generateKey(
  { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]),
    hash: "SHA-256" }, true, ["sign", "verify"]);
const jwk = { ...(await crypto.subtle.exportKey("jwk", publicKey)), kid: "k1" };
const getKeys = async () => [jwk];

const b64url = (bytes) => Buffer.from(bytes).toString("base64url");
const now = Math.floor(Date.now() / 1000);
async function token(claims = {}, { key = privateKey, kid = "k1", alg = "RS256" } = {}) {
  const head = b64url(JSON.stringify({ alg, kid, typ: "JWT" }));
  const body = b64url(JSON.stringify({ aud: [aud], iss: `https://${team}`, exp: now + 600,
    iat: now, email: "someone@example.com", ...claims }));
  const sig = await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, new TextEncoder().encode(`${head}.${body}`));
  return `${head}.${body}.${b64url(new Uint8Array(sig))}`;
}
const check = (t, opts = {}) => verifyAccessJwt(t, { teamDomain: team, aud, getKeys, ...opts });

const cases = [
  ["valid token", async () => assert.equal((await check(await token())).email, "someone@example.com")],
  ["no token", async () => assert.rejects(check(null), /no Access token/)],
  ["not configured", async () => assert.rejects(check(await token(), { aud: "" }), /not configured/)],
  ["malformed", async () => assert.rejects(check("abc.def"), /malformed/)],
  ["wrong audience", async () => assert.rejects(check(await token({ aud: ["other-app"] })), /wrong audience/)],
  ["wrong issuer", async () => assert.rejects(check(await token({ iss: "https://evil.cloudflareaccess.com" })), /wrong issuer/)],
  ["expired", async () => assert.rejects(check(await token({ exp: now - 1 })), /expired/)],
  ["not yet valid", async () => assert.rejects(check(await token({ nbf: now + 3600 })), /not yet valid/)],
  ["signed by another key", async () => assert.rejects(check(await token({}, { key: other.privateKey })), /bad signature/)],
  ["unknown kid", async () => assert.rejects(check(await token({}, { kid: "k2" })), /unknown signing key/)],
  ["alg none", async () => assert.rejects(check(await token({}, { alg: "none" })), /unexpected alg/)],
  ["tampered claims", async () => {
    const [h, , s] = (await token()).split(".");
    const forged = b64url(JSON.stringify({ aud: [aud], iss: `https://${team}`, exp: now + 99999 }));
    await assert.rejects(check(`${h}.${forged}.${s}`), /bad signature/);
  }],
];
let failed = 0;
for (const [name, fn] of cases) {
  try { await fn(); console.log(`ok   ${name}`); }
  catch (e) { failed++; console.log(`FAIL ${name}: ${e.message}`); }
}
console.log(`${cases.length - failed}/${cases.length} passed`);
process.exit(failed ? 1 : 0);
