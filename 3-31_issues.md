# SERP to Prompt Writer — Audit Findings (2026-03-31)

Audited all 30 sessions in the database from the most recent batch run.

---

## Session Ratings

### Good (8-9/10)
- `symptoms of dehydration` — top 30 almost flawless, tight semantic relevance
- `easy yoga poses for beginners` — clean multi-word phrases, all on-topic
- `how to make homemade pizza dough` — excellent recipe keywords
- `best gaming laptops under 1000` — brands + specs + variants, great
- `how to train a puppy` — clean topical phrases
- `how to make kombucha at home` — domain-specific terms surface well

### Decent (6-7/10)
- `best budget fitness trackers` — good brands at top, junk in tail
- `best electric scooters for adults` — "lbs battery capacity" spec fragments clog mid-range
- `natural supplements for anxiety` — solid but academic citation junk leaks (doi, pmc)
- Most of the "best X" product queries fall here

### Problem (4-5/10)
- `easy ways to reduce stress` — **worst session**: `htm` at #8, `exp` at #28, author names (scholey, aronsson, ragland), org names (nccih, cloudflare, hopkinsmedicine), truncated words (hysical, eview, booket), 718 keywords junked but plenty still leaked
- `signs of burnout at work` — 181 junked, 3 low-BERT in top 50
- `easy yoga poses for beginners` — 577 junked (top 30 is clean but long tail is bad)

---

## Per-Session Stats

| Session | Intent | Active KW | Entities | Headings | PAA | Junked KW | Low-SEM Top50 |
|---------|--------|-----------|----------|----------|-----|-----------|---------------|
| best budget fitness trackers | commercial/comparison | 1957 | 0 | 98 | 0 | 43 | 0 |
| how to make kombucha at home | informational/recipe | 1124 | 2 | 90 | 0 | 32 | 0 |
| best wireless mice for laptop | commercial/listicle | 1110 | 0 | 78 | 3 | 194 | 1 |
| symptoms of dehydration | informational/health guide | 1083 | 0 | 88 | 0 | 238 | 3 |
| how to fix slow internet | informational/article | 1710 | 1 | 89 | 0 | 111 | 0 |
| best noise machines for sleep | commercial/listicle | 713 | 2 | 89 | 1 | 148 | 0 |
| easy crockpot chicken recipes | informational/recipe | 633 | 0 | 52 | 1 | 22 | 0 |
| how to start journaling | informational/how-to | 1534 | 0 | 97 | 2 | 96 | 0 |
| best electric toothbrushes | commercial/comparison | 1851 | 0 | 94 | 1 | 149 | 0 |
| natural remedies for constipation | informational/article | 1215 | 0 | 98 | 0 | 236 | 0 |
| how to build muscle at home | informational/article | 1335 | 0 | 100 | 1 | 79 | 1 |
| best portable chargers 2026 | commercial/listicle | 1750 | 0 | 95 | 0 | 231 | 0 |
| signs of early dementia | informational/article | 990 | 4 | 59 | 1 | 260 | 2 |
| how to clean stainless steel appliances | informational/article | 828 | 1 | 92 | 1 | 105 | 0 |
| best blenders for smoothies | commercial/comparison | 1672 | 0 | 94 | 2 | 328 | 0 |
| easy ways to reduce stress | informational/article | 1282 | 0 | 79 | 1 | 718 | 16 |
| how to make homemade pizza dough | informational/recipe | 1111 | 1 | 55 | 1 | 39 | 0 |
| best standing desks for home office | commercial/listicle | 815 | 0 | 83 | 3 | 51 | 0 |
| symptoms of food allergies | informational/health guide | 1044 | 0 | 76 | 2 | 158 | 0 |
| how to remove rust from metal | informational/article | 1274 | 1 | 97 | 1 | 47 | 0 |
| best gaming laptops under 1000 | commercial/listicle | 1204 | 0 | 93 | 0 | 205 | 0 |
| natural supplements for anxiety | informational/listicle | 1096 | 1 | 78 | 0 | 541 | 4 |
| how to train a puppy | informational/article | 1006 | 0 | 95 | 0 | 132 | 0 |
| best robot vacuums 2026 | informational/listicle | 1562 | 0 | 95 | 0 | 438 | 1 |
| signs of burnout at work | informational/article | 1819 | 0 | 95 | 0 | 181 | 3 |
| how to save money on groceries | informational/article | 1181 | 2 | 96 | 0 | 211 | 2 |
| best mattresses for back pain | informational/listicle | 1555 | 1 | 90 | 0 | 445 | 0 |
| easy yoga poses for beginners | informational/guide | 1035 | 2 | 68 | 4 | 577 | 8 |
| how to make cold brew coffee | informational/how-to | 1076 | 0 | 93 | 0 | 51 | 0 |
| best electric scooters for adults | commercial/comparison | 1680 | 1 | 94 | 0 | 236 | 6 |

---

## Issue 1: Broken Scrapes Polluting Data

**122 broken scrapes across all 30 sessions. Every single session is affected.**

Pages with <50 words but 10-200 headings are pure navigation/template markup. They pump junk headings into the corpus while contributing nothing to NLP. Recipe sites are the worst offenders (recipe cards render via JS, leaving only nav headings).

Examples:
- garagegymreviews.com: 10 words, 200 headings
- naplab.com: 10 words, 115 headings
- mattressnerd.com: 37 words, 132 headings
- skinnytaste.com: 8 words, 57 headings
- strengthlog.com: 11 words, 91 headings
- techradar.com: 36-42 words, 66-85 headings (multiple sessions)
- vacuumwars.com: 5 words, 69 headings

**Impact**: Heading patterns get diluted with site chrome. Word count stats get skewed (min_words=56 in fitness trackers is from a broken page). These pages should be excluded from heading analysis and content stats entirely.

---

## Issue 2: NER Is Broken (18/30 Sessions = 0 Entities)

Sessions with entities: 12/30 (and most of those have only 1-4 entities)
Sessions with 0 entities: 18/30

The log shows `ONNX NER crashed on page 0 (cnet.com) -- skipping` — when NER crashes on one page, it appears to abandon the entire session's entity extraction rather than continuing to the next page.

---

## Issue 3: PAA Frequently Empty (15/30 Sessions = 0 PAA)

Half the sessions have no People Also Ask data. This is a Serper API issue — they don't always return PAA results. The prompt template has a PAA section that renders empty for these sessions.

---

## Issue 4: Cross-Session Junk Keywords

Words appearing in the top-200 of 3+ unrelated sessions — these are systemic, not topic-specific:

### Review Template / Editorial Chrome (add to chrome_words[])
| Word | Sessions | Notes |
|------|----------|-------|
| specs | 8 | "specs and features" review template |
| cons | 8 | pros/cons template |
| testers | 8 | "our testers found" |
| tester | 8 | same |
| pros | 5 | pros/cons template |
| excels | 4 | "this product excels" |
| testimonials | 3 | site chrome |
| roundup | in output | "our roundup" review template |
| unboxing | in output | YouTube/review junk |
| paywall | in output | publishing junk |

### Editorial Filler (add to chrome_words[])
| Word | Sessions | Notes |
|------|----------|-------|
| pricier | 8 | comparative adjective |
| seamlessly | 5 | adverb filler |
| thankfully | 3 | adverb filler |
| classy | in output | subjective adjective |
| refreshed | in output | "refreshed design" editorial |
| shiniest | in output | superlative filler |
| clunky | in output | editorial adjective |
| impressively | in output | adverb filler |
| wholeheartedly | in output | adverb filler |
| bevy | in output | "a bevy of features" |
| buffs | in output | "fitness buffs" slang |
| fooled | in output | "don't be fooled" editorial |
| whistles | 3 | "bells and whistles" |
| pluses | in output | "pluses and minuses" |

### Web/UI Terms (add to chrome_words[])
| Word | Sessions | Notes |
|------|----------|-------|
| inbox | 5 | email/newsletter |
| tabs | in output | UI element |
| chatbot | in output | off-topic tech |
| browse | 3 | site navigation |
| opt | 6 | "opt in/out" filler |

### Retail/Filler (add to chrome_words[])
| Word | Sessions | Notes |
|------|----------|-------|
| amazon | 5 | retailer name (single word bypasses n-gram filter) |
| basics | 6 | "back to basics" filler |
| fridge | 6 | cross-sell bleed across unrelated topics |
| tasty | 5 | editorial adjective |
| usability | 5 | editorial noun |
| msrp | in output | retail jargon |

---

## Issue 5: Academic/Citation Junk (Health Topics)

Health authority sites (NHS, NIH, Mayo Clinic, PubMed) contain academic citation markup that leaks into keywords. These score well on cfr because they're not in wiki_freq (get the 0.85 default "niche term" score).

| Word | Sessions | What It Is |
|------|----------|-----------|
| htm | 1 (#8!) | URL fragment (.htm) |
| exp | 1 (#28) | URL/citation fragment |
| doi | 1 (#129) | Digital Object Identifier |
| ncbi | 2 | National Center for Biotechnology Information |
| pmc | 1 | PubMed Central |
| nih | 2 | National Institutes of Health |
| nccih | 1 | NIH complementary health center |
| eview | 1 | truncated "review" |
| addbeh | 1 | journal abbreviation (Addictive Behaviors) |
| freephone | 1 | UK telephone junk |
| helpline | 2 | support line chrome |
| webchat | 1 | site chrome |

---

## Issue 6: Publisher/Site Names Leaking as Single Words

The n-gram publisher filter catches "fryer amazon" but standalone single-word publisher names bypass it:

| Word | Sessions | Notes |
|------|----------|-------|
| forbes | in output (#235) | publisher, already in n-gram filter but not chrome_words[] |
| cloudflare | 1 | CDN/infrastructure |
| hopkinsmedicine | 1 | site name fragment |
| helpguide | 1 | site name |
| stepchange | 1 | UK charity site |

---

## Issue 7: Author/Journalist Names (Single Word)

The proper name filter only catches multi-word "Firstname Lastname" capitalized patterns. Single-word names bypass it:

| Word | Session | Notes |
|------|---------|-------|
| gebhart | best budget fitness trackers | journalist surname |
| moscaritolo | best budget fitness trackers | PCMag writer |
| scholey | easy ways to reduce stress | researcher surname |
| aronsson | easy ways to reduce stress | researcher surname |
| ragland | easy ways to reduce stress | author surname |
| shruthi | easy ways to reduce stress | author first name |
| gottesman | signs of early dementia | researcher surname |
| andrew | best budget fitness trackers | first name from bylines |
| angela | best budget fitness trackers | first name from bylines |
| scott | best budget fitness trackers | first name from bylines |
| stocksy | symptoms of dehydration | stock photo site name |

---

## Issue 8: Truncated/Fragment Tokens

Text parsing artifacts from broken Unicode handling, URL stripping, or word boundary issues:

| Word | Session | What It Should Be |
|------|---------|------------------|
| hysical | easy ways to reduce stress | "physical" (truncated) |
| eview | easy ways to reduce stress | "review" (truncated) |
| booket | easy ways to reduce stress | "booklet" (truncated) |
| spo | best budget fitness trackers | "SpO2" (oxygen saturation) |
| jan | best budget fitness trackers | "January" (month fragment) |
| malformed | easy ways to reduce stress | possibly genuine but likely HTML error text |

---

## Issue 9: Heading Pattern Junk

Headings that made it through the blacklist into the final prompt output:

### Appearing in multiple sessions
| Heading | Sessions | Notes |
|---------|----------|-------|
| Our Top Tested Picks | 4 | review template |
| Sections | 2 | nav element |
| Our top picks | 2 | review template |
| Warner Bros. Discovery Sets... | 2 | CNN news ticker bleed |

### Appearing in single sessions but clearly junk
- "Thank you for registering" (2x in fitness trackers) — registration chrome
- "Deals" (2x in fitness trackers) — nav section
- "Follow today" — social CTA
- "Stay up to date with notifications from TheIndependent" — newsletter CTA
- "Your cart (0)" / "Your cart is empty" — ecommerce chrome
- "Just added to your cart" — ecommerce chrome
- "Why Trust The Spruce Eats" / "Why trust Reviewed?" / "Why Trust Forbes Vetted" — trust badges
- "Contact information" / "Contact Doctor During Office Hours" / "Need help? Call us on..." — contact chrome
- "Disclosures" / "Privacy & Security" — legal chrome
- "Financial Tips" — site section nav
- "Do" (2x in reduce stress) — broken heading text
- "Gaming Laptop Deals Under $1,000" / "Best Mattress Deals Happening Now" — deals chrome
- "CES 2026: All of the big robot vacuum announcements..." — news article bleed

### Recommended additions to heading blacklist
```
"thank you for registering", "deals", "our top tested picks",
"our top picks", "top picks at a glance", "your cart",
"just added to your cart", "why trust", "contact information",
"need help", "financial tips", "do", "gaming laptop deals",
"mattress deals", "best deals", "deals happening",
"latest news", "latest stories"
```

---

## Issue 10: Prompt Structure Problems

Observed in the "best budget fitness trackers" prompt (likely affects others):

1. **"Primary keyword phrases:" is empty** — no multi-word keywords populate the primary slot
2. **All outline headings are H3** with no H2 parent — structural issue
3. **Word range min is from broken scrapes** — "56 - 8715" where 56 comes from a page that failed to render

---

## Recommended Fix Priority

1. **Exclude broken scrapes from heading + stats analysis** — pages with <50 words should not contribute headings or affect word count stats. Biggest single quality improvement.
2. **Add cross-session junk to chrome_words[]** — the 30+ words identified in Issue 4 appear repeatedly.
3. **Add academic/citation fragments to chrome_words[]** — htm, exp, doi, ncbi, pmc, nih, nccih, eview, addbeh, freephone, helpline, webchat.
4. **Add publisher names to chrome_words[]** — forbes, cloudflare, hopkinsmedicine, helpguide, stepchange.
5. **Fix NER crash recovery** — when ONNX NER crashes on one page, continue to the next instead of aborting.
6. **Expand heading blacklist** — add the patterns listed in Issue 9.
7. **Consider single-word author name detection** — words with cfr=0.85 (not in wiki) and sem<0.15 that don't match known brand/product patterns could be flagged.
