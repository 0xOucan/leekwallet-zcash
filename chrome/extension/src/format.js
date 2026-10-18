// Amounts and history lines as the popup shows them. No DOM, so the Node
// tests import it (test/format.test.mjs).

export function formatZec(zat) {
  const z = BigInt(zat ?? 0);
  const neg = z < 0n;
  const a = neg ? -z : z;
  const s = `${a / 100000000n}.${(a % 100000000n).toString().padStart(8, '0')}`;
  return `${neg ? '-' : ''}${s} ZEC`;
}

/**
 * One line of the history: `{ label, amount }`. A send to the wallet's own
 * address pays no one else, and while the engine knows its fee (before it
 * is mined) the amount sent is the balance change minus that fee: zero.
 * Then the line shows the balance change itself, the fee it cost.
 */
export function historyEntry(t) {
  const when = t.minedHeight ? ` @${t.minedHeight}` : ' (unmined)';
  if (t.kind === 'sent' && t.amountZat === 0 && t.deltaZat < 0 && !t.recipients?.length) {
    return { label: `sent to yourself${when}`, amount: formatZec(t.deltaZat) };
  }
  return { label: `${t.kind}${when}`, amount: `${t.deltaZat < 0 ? '-' : '+'}${formatZec(t.amountZat)}` };
}
