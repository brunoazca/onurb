---
name: shopping
description: "Buy a physical item for the user: research the main retailers, pick the single best option yourself, stage it at checkout and propose it on the watch (photo, title, total, considerations). The order is placed only after the user taps Confirm."
---

# Shopping (Onurb)

Adapted from the user's Shopping Scout skill (Claude Cowork). Differences: you make the final choice
yourself (no top-10 table for the user), and you stage the purchase so the user can buy it with one
tap on the watch. **You never place the order.** A separate checkout task does that, and only
after the user confirms on the watch.

## Fixed context

- Ship-to and ZIP: in your system prompt ("Shipping address"). Set that ZIP on every site before
  reading any delivery date; dates from an unset ZIP are worthless.
- Browser: Claude in Chrome, the **"Linux"** browser (the Onurb server's Chrome). The stores and the
  user's card are set up there. Never use the "macOS" browser for shopping.
- Currency USD. Include tax if the site shows it; always include shipping.
- Per-purchase ceiling: in your system prompt. Anything whose checkout total is above it cannot be
  proposed: pick the best option under it, or report that nothing fits.

## Step 1: Scope (no questions unless it changes everything)

Infer quantity/size, hard specs, condition (new unless the user said otherwise) and urgency from the
request. Ask the user only if the request is genuinely ambiguous (e.g. "a charger" for an unknown
device): then end the task with the question instead of guessing.

## Step 2: Sites

Always Amazon and Walmart. Add Target and the category fit: electronics → Best Buy, B&H; household
→ Target, Costco; home → IKEA, Wayfair; tools → Home Depot; supplements → iHerb, brand store;
books → Bookshop.org. Skip Temu for branded, safety-critical or ingested items. Prefer sites the
Linux browser is logged into (the purchase must be completed there); check that early.

## Step 3: Search (thorough, but this is a watch request: aim for ~30-60 listings)

Per main site: 2 query phrasings, sort by relevance and by price, open the product page of every
serious candidate (grid prices and dates are often stale), check Amazon "Other sellers" and
Walmart marketplace sellers. Use `get_page_text` to read; screenshots only when the visual matters.

## Step 4: Capture per candidate (URL at scan time)

Title, site, exact seller, item price, shipping, total, quantity, unit price, delivery date, condition,
return window, rating + review count, **direct URL** (collect hrefs with `javascript_tool` while the
results page is open; never reconstruct or guess a URL/ASIN), and the **main product image URL**
(the product page's `og:image` meta or main image `src`).

## Step 5: Condition and counterfeit check

Same rules as Shopping Scout: Amazon not sold/shipped by Amazon, commingled ASINs, alphabet-soup
sellers, review hijacking → raise risk; Walmart marketplace sellers checked; Temu branded = fake;
price under ~50-60% of MSRP for a brand that doesn't discount → High. High-counterfeit categories
(memory cards, SSDs, chargers, cables, batteries, headphones, ink, cosmetics, supplements, LEGO,
tools): prefer authorized sellers even at a premium. **Safety-critical** (chargers, batteries,
helmets, car seats, detectors, wiring, anything ingested/applied to skin, anything for a child):
never pick an unbranded or High-risk listing.

## Step 6: Score and decide

```
d     = delivery days − delivery days of the fastest viable candidate   (cap 30)
score = unit_price_total × (1 + rate × d)     # rate 0.05/day; 0.10-0.15 if urgent; 0.01-0.02 if no rush
```

Do the arithmetic in a script. Drop High-risk and safety-failing listings first. **Pick the lowest
score** unless a clear reason overrides it (much better seller, return policy, rating); if so, say
why in the considerations.

## Step 7: Verify and stage at checkout

1. Re-open the pick's URL: confirm price, seller, stock, delivery date for the ZIP.
2. Add exactly the chosen quantity to the cart (remove anything else already in that cart; tell the
   user if you removed items) and open the checkout **review** page.
3. Read the exact total (item + shipping + tax), the shipping address and the payment method shown.
   The address must match the ship-to; the payment must be a saved card. If the site wants a new
   card, a CVC, a login or a captcha, stop: report what the user must do once on that site.
4. **Stop here. Do not click "Place order" / "Buy now" / "Pay".**

## Step 8: Propose on the watch

Call `propose_purchase` with:
- `title`: short product name (brand + model + size/qty), ≤ 70 characters.
- `site`, `url` (the product page), `image_url` (main product image).
- `total_usd`: the checkout total including shipping and tax.
- `details`: 2-5 short lines for the watch, e.g. "Chega qui 14/11", "Vendido pela Amazon",
  "4.6★ (12k)", and your considerations: why this one beat the others, risks, what you assumed.
  Portuguese, no emoji.

Then finish with the result: what you picked, the runner-up and why it lost, how many listings you
scanned, and `SPOKEN: <one sentence: what you found and that it is on the watch to confirm>`.

## Guardrails

- Never place an order, never type card numbers, CVCs, passwords or one-time codes.
- Never invent a price, date, review count, URL or image URL.
- Captcha or bot block: note it and move on; don't fight it.
- Stop and report after 2-3 failed attempts on a site.
