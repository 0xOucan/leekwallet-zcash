// The popup's amounts and history lines (src/format.js), in Node.
//
//   cd chrome/extension && node --test test/format.test.mjs
import assert from 'node:assert/strict';
import { test } from 'node:test';

import { formatZec, historyEntry } from '../src/format.js';

test('formatZec: exact, signed, 8 decimals', () => {
  assert.equal(formatZec(10_000), '0.00010000 ZEC');
  assert.equal(formatZec(-123_456_789), '-1.23456789 ZEC');
  assert.equal(formatZec(0), '0.00000000 ZEC');
});

test('a send to yourself shows the fee it cost, not -0', () => {
  // 0.0001 ZEC to the wallet's own address, fee 0.0001, just broadcast:
  // the engine knows the fee and takes it off what was sent, leaving 0.
  const pending = { kind: 'sent', minedHeight: null, deltaZat: -10_000, amountZat: 0, feeZat: 10_000, recipients: [] };
  assert.deepEqual(historyEntry(pending), { label: 'sent to yourself (unmined)', amount: '-0.00010000 ZEC' });
  const mined = { ...pending, minedHeight: 4_500_123 };
  assert.deepEqual(historyEntry(mined), { label: 'sent to yourself @4500123', amount: '-0.00010000 ZEC' });
});

test('other lines are as before', () => {
  const sent = { kind: 'sent', minedHeight: 4_500_000, deltaZat: -410_000, amountZat: 400_000, recipients: [{ address: 'utest1x', valueZat: 400_000 }] };
  assert.deepEqual(historyEntry(sent), { label: 'sent @4500000', amount: '-0.00400000 ZEC' });
  const received = { kind: 'received', minedHeight: 4_500_001, deltaZat: 400_000, amountZat: 400_000, recipients: [] };
  assert.deepEqual(historyEntry(received), { label: 'received @4500001', amount: '+0.00400000 ZEC' });
  // The engine already gave an amount (the fee unknown, say): unchanged.
  const known = { kind: 'sent', minedHeight: 4_500_002, deltaZat: -10_000, amountZat: 10_000, recipients: [] };
  assert.deepEqual(historyEntry(known), { label: 'sent @4500002', amount: '-0.00010000 ZEC' });
});
