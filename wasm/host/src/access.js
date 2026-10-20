// Cloudflare Access token check (decision 10). Access already stops anyone not on the allow-list
// before the Worker runs; this is the second barrier, in case the Access application is ever
// misconfigured or detached. Every request must carry a valid Access JWT for this application:
// signed (RS256) by one of the team's current keys, for our audience tag, from our team, unexpired.

const certCache = new Map(); // team domain -> { keys, expires }

// The team's signing keys, cached for an hour (Access rotates them every six weeks).
export async function fetchTeamKeys(teamDomain, fetchImpl = fetch) {
  const hit = certCache.get(teamDomain);
  if (hit && hit.expires > Date.now()) {
    return hit.keys;
  }
  const res = await fetchImpl(`https://${teamDomain}/cdn-cgi/access/certs`);
  if (!res.ok) {
    throw new Error(`Access certs: HTTP ${res.status}`);
  }
  const { keys } = await res.json();
  certCache.set(teamDomain, { keys, expires: Date.now() + 3600_000 });
  return keys;
}

function b64urlBytes(s) {
  const b64 = s.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((s.length + 3) % 4);
  return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
}

function b64urlJson(s) {
  return JSON.parse(new TextDecoder().decode(b64urlBytes(s)));
}

/**
 * Returns the token's claims if it is valid, or throws with the reason.
 * @param token   the Cf-Access-Jwt-Assertion header
 * @param options { teamDomain: "<team>.cloudflareaccess.com", aud, getKeys?: () => JWK[], now? }
 */
export async function verifyAccessJwt(token, { teamDomain, aud, getKeys, now = Date.now() }) {
  if (!teamDomain || !aud) {
    throw new Error("ACCESS_TEAM_DOMAIN / ACCESS_AUD not configured");
  }
  if (!token) {
    throw new Error("no Access token");
  }
  const parts = token.split(".");
  if (parts.length !== 3) {
    throw new Error("malformed token");
  }
  const header = b64urlJson(parts[0]);
  const claims = b64urlJson(parts[1]);
  if (header.alg !== "RS256") {
    throw new Error(`unexpected alg ${header.alg}`);
  }
  const keys = await (getKeys ? getKeys() : fetchTeamKeys(teamDomain));
  const jwk = keys.find((k) => k.kid === header.kid);
  if (!jwk) {
    throw new Error("unknown signing key");
  }
  const key = await crypto.subtle.importKey(
    "jwk", { kty: jwk.kty, n: jwk.n, e: jwk.e, alg: "RS256", ext: true },
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["verify"]);
  const signed = new TextEncoder().encode(`${parts[0]}.${parts[1]}`);
  if (!(await crypto.subtle.verify("RSASSA-PKCS1-v1_5", key, b64urlBytes(parts[2]), signed))) {
    throw new Error("bad signature");
  }
  const auds = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
  if (!auds.includes(aud)) {
    throw new Error("wrong audience");
  }
  if (claims.iss !== `https://${teamDomain}`) {
    throw new Error("wrong issuer");
  }
  const t = Math.floor(now / 1000);
  if (typeof claims.exp !== "number" || claims.exp <= t) {
    throw new Error("expired");
  }
  if (typeof claims.nbf === "number" && claims.nbf > t + 60) {
    throw new Error("not yet valid");
  }
  return claims;
}
