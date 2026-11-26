// npm run set-password  -> prompts for a new password and stores its scrypt hash in data/auth.json.
// Restart the server afterwards. SM_PASSWORD, if set, still takes precedence.
import readline from "node:readline/promises";
import { DATA_DIR, setPassword } from "./auth.js";

const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
const a = await rl.question("New password (min 12 chars): ");
const b = await rl.question("Repeat: ");
rl.close();
if (a !== b) { console.error("Passwords differ."); process.exit(1); }
if (a.length < 12) { console.error("Too short."); process.exit(1); }
await setPassword(a);
console.log(`Saved hash to ${DATA_DIR}/auth.json. Restart the server.`);
