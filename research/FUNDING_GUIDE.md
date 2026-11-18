# WetGreg — Funding & Path-to-Production Guide

How to get WetGreg from a working prototype to a funded, manufacturable product,
and when it becomes safe to work on it full time.

> **Read this first.** This guide was written from general knowledge on
> 2026-11-16, not from a live search of current programs. Program terms, fees
> and eligibility change — verify each one on its own site before relying on it.
> All dollar figures are rough planning estimates, not quotes. It assumes a
> US-based solo founder.

---

## Table of contents

1. [The short answer](#1-the-short-answer)
2. [What WetGreg is, from a funder's point of view](#2-what-wetgreg-is-from-a-funders-point-of-view)
3. [Funding avenues that fit](#3-funding-avenues-that-fit)
   - [3.1 Small-batch direct sales](#31-small-batch-direct-sales)
   - [3.2 Crowdfunding](#32-crowdfunding)
   - [3.3 Hardware accelerators](#33-hardware-accelerators)
   - [3.4 Angels, friends and family](#34-angels-friends-and-family)
   - [3.5 Contests and maker prizes](#35-contests-and-maker-prizes)
4. [Avenues that mostly don't fit](#4-avenues-that-mostly-dont-fit)
   - [4.1 Government R&D grants](#41-government-rd-grants)
   - [4.2 AI startup programs](#42-ai-startup-programs)
   - [4.3 Venture capital](#43-venture-capital)
5. [Where the AI-assisted development story helps](#5-where-the-ai-assisted-development-story-helps)
6. [What production actually costs](#6-what-production-actually-costs)
7. [The plan, step by step](#7-the-plan-step-by-step)
8. [When to quit the day job](#8-when-to-quit-the-day-job)
9. [Common ways hardware projects fail](#9-common-ways-hardware-projects-fail)
10. [Next actions checklist](#10-next-actions-checklist)

---

## 1. The short answer

- No program pays a salary to finish a consumer gadget. Grants fund research;
  investors fund traction.
- "Built with AI" is not a funding category. It makes you faster and cheaper,
  which strengthens a pitch, but nobody funds it on its own.
- The realistic money is **customers pre-ordering the device** — a small
  hand-built batch first, then a crowdfunding campaign.
- Keep the job until demand is proven. The campaign goal should include your
  living costs, so that a funded campaign is what lets you quit.

---

## 2. What WetGreg is, from a funder's point of view

| Aspect | Today | Why it matters |
|---|---|---|
| Product | E-ink companion pet: moods, quotes, pedometer, story about Greg combating loneliness | Consumer gadget / toy category, not deep tech |
| Hardware | Pico 2 W (RP2350) on a custom PCB, 2.13" e-ink, accelerometer, battery | Cheap parts, pre-certified radio module, hackable |
| Firmware | FreeRTOS, OTA-capable, localization (English, Egyptian Arabic), two quote editions | Working product, not a concept |
| Stage | Functional prototype, no sales yet | Too early for investors, right stage for audience building |
| Team | Solo founder, AI-assisted development | Low burn rate; also a single point of failure |

Strengths to lead with: it works today, it has personality, e-ink gives long
battery life and a distinctive look, and the adult "filthy" edition is a
ready-made marketing hook.

Gaps a funder will ask about: enclosure design, per-unit cost at volume,
evidence that strangers want it, and what stops a clone.

---

## 3. Funding avenues that fit

### 3.1 Small-batch direct sales

Hand-build 20–50 units and sell them.

- **Where:** Tindie (maker hardware), Etsy (gift buyers), or your own site
  with Shopify or similar.
- **Why first:** it is the cheapest proof of demand, and it surfaces the
  assembly, shipping and support problems while they are still small.
- **What you learn:** a real price people pay, return and defect rates, which
  features buyers mention, and testimonials for the campaign page.
- **Money:** it will not pay your rent. Its job is evidence.

### 3.2 Crowdfunding

The main avenue for this kind of product.

| Platform | Best for | Notes |
|---|---|---|
| **Crowd Supply** | Open, hackable hardware | Vets projects, helps with fulfilment, audience likes Pico-class devices. Smaller audience, higher trust. |
| **Kickstarter** | Mass-market gadgets and toys | Largest audience, but you must bring your own crowd. All-or-nothing funding. |
| **Indiegogo** | Follow-on / late pre-orders | Commonly used after a first campaign rather than as the first. |

Rules of thumb:

- Collect roughly **1,000+ email signups** before launching. A small share of
  a warm list converts on day one, and early momentum drives the platform's
  own promotion.
- Platform and payment fees take roughly 8–10% of the raise.
- Set the goal from costs, not hope: manufacturing run + certification +
  shipping + fees + contingency + your living costs.
- Offer few reward tiers. One device, one bundle, one early-bird price.

### 3.3 Hardware accelerators

- **HAX (SOSV)** — the best-known hardware accelerator. Cash plus
  manufacturing and supply-chain help, in exchange for equity.
- **Y Combinator** — accepts hardware, but rarely before there is traction.
- **Local and regional programs** — many cities and universities run
  incubators with small non-dilutive stipends, shared lab space and mentors.
  Search your state's economic development agency and nearest university.

These become realistic after small-batch sales or a strong signup list.

### 3.4 Angels, friends and family

- Plausible once you have sales numbers or a funded campaign.
- Use a standard instrument (a SAFE or convertible note) rather than an
  improvised agreement, and get a lawyer to review it.
- Taking money from people you know puts those relationships at risk if the
  product fails. Take only what they can afford to lose.

### 3.5 Contests and maker prizes

- The Hackaday Prize and similar maker contests award cash and attention.
- Chip and distributor vendors sometimes run design contests.
- The prize money is modest. The publicity and the credibility line on a
  campaign page are the real value.

---

## 4. Avenues that mostly don't fit

### 4.1 Government R&D grants

SBIR/STTR and NSF seed funding pay for novel R&D with technical risk. A
companion pet on off-the-shelf parts does not qualify as written.

The loneliness angle could open health or wellbeing grants, but that requires
a research partner (typically a university), a study design and outcome
evidence. That is a slower and different project, and worth considering only
if you actually want WetGreg to be a wellbeing intervention.

### 4.2 AI startup programs

Programs from Anthropic, Microsoft, Google and AWS provide cloud or API
credits, not living money. They are worth applying for only if WetGreg gains
a cloud or AI feature that consumes those credits.

### 4.3 Venture capital

VCs avoid single-product consumer hardware before there is revenue. Margins
are thin and inventory risk is high. Revisit only if sales show a path to a
product line or recurring revenue.

---

## 5. Where the AI-assisted development story helps

It is not a funding source, but it is useful in three places:

1. **The pitch.** A solo founder who ships firmware, localization and tooling
   at this pace needs less money to reach production. State it as burn rate:
   "one person, no engineering payroll."
2. **Content marketing.** Build logs about making a hardware product with AI
   assistance draw an audience on Hackaday, Reddit, YouTube and Hacker News.
   That audience becomes the email list.
3. **Accelerator applications.** It answers the "why can one person do this?"
   question.

Be ready for the follow-up: reviewers will ask whether you understand and can
maintain the code. The requirements docs and development guide in this repo
are good evidence that you can.

---

## 6. What production actually costs

Rough planning ranges for a first run of 500–1,000 units. Get real quotes
before setting a campaign goal.

| Item | Rough range | Notes |
|---|---|---|
| Electronics BOM per unit | $15–30 | Pico 2 W, e-ink panel, accelerometer, battery, PCB, passives |
| PCB assembly per unit | $3–8 | Falls with volume |
| Enclosure tooling (one-off) | $3,000–15,000 | Injection mould. 3D printing or resin casting avoids this for small runs |
| Enclosure per unit | $1–5 moulded, $5–15 printed | |
| FCC / regulatory testing | $2,000–6,000 | Lower because the Pico 2 W radio module is pre-certified; unintentional-emissions testing still applies |
| Battery shipping compliance | varies | Lithium cells need UN38.3 test documentation, usually supplied by the cell vendor |
| Packaging per unit | $1–3 | |
| Shipping and fulfilment per unit | $5–15 domestic | International adds duties and VAT handling |
| Returns, defects, contingency | 10–20% of total | Do not skip this |

Other points:

- A common target is a retail price of **3–4× the landed unit cost**.
- If the product is marketed to children, US child-product safety rules add
  testing. The adult edition avoids that; decide the audience deliberately.
- Moving from the soldered-on Pico 2 W to a bare RP2350 plus radio module
  lowers unit cost at volume but adds design and certification work. It is not
  needed for a first run.
- Form an LLC before taking money, and budget for a bookkeeper. Crowdfunding
  proceeds are generally taxable income.

---

## 7. The plan, step by step

All of this is done while still employed.

| Phase | Goal | Done when |
|---|---|---|
| **1. Story** | Polish the intro video; landing page with email signup | Page is live and collecting addresses |
| **2. Audience** | Regular build logs and short videos | ~1,000 signups |
| **3. Small batch** | Sell 20–50 hand-built units | Sold out, feedback collected |
| **4. Costing** | Quotes for a 500–1,000 unit run, enclosure included | Landed unit cost known to within ~15% |
| **5. Campaign prep** | Apply to Crowd Supply or build the Kickstarter page; working near-final prototypes for photos and reviewers | Page approved, launch date set |
| **6. Campaign** | Run it | Funded |
| **7. Manufacture and ship** | Deliver to backers | Rewards shipped |
| **8. Ongoing sales** | Own store, restocks, next edition | Monthly revenue covers costs |

Where to post in phase 2: r/raspberrypipico, r/eink, r/tamagotchi,
r/virtualpets, Hackaday.io, TikTok and YouTube Shorts.

Firmware items in this repo that matter before selling units: the battery
gauge and power budget, the set-time controls, menu clarity, and the USB
updater utility listed in `Next Steps.md`. Buyers need to update the device
without a toolchain.

---

## 8. When to quit the day job

Quit when one of these is true:

- A campaign has funded with your living costs for 6–12 months inside the
  goal, **or**
- You have 6–12 months of personal savings **and** clear demand evidence
  (a sold-out small batch and a growing list).

Before then, options that reduce risk without stopping progress:

- Negotiate part-time hours or a sabbatical.
- Take contract work for income while building.
- Set a fixed weekly schedule for WetGreg so progress does not depend on
  spare energy.

Check any employment agreement for intellectual-property and moonlighting
clauses before selling anything, especially if your employer could claim work
done on personal time.

---

## 9. Common ways hardware projects fail

- **Underpricing.** Shipping, duties, fees and defects eat the margin.
- **Launching without an audience.** Campaigns that start cold rarely fund.
- **Too many variants.** Each colour or edition multiplies inventory risk.
- **Late delivery.** Tooling and certification take longer than planned;
  promise conservative dates.
- **No support plan.** Decide how updates, returns and dead units are handled
  before shipping the first one.
- **Quitting too early.** Running out of personal money ends more projects
  than technical problems do.

---

## 10. Next actions checklist

- [ ] Finish the intro video
- [ ] Put up a landing page with an email signup
- [ ] Start a weekly build log on two channels
- [ ] Build the per-unit cost spreadsheet from the current parts
- [ ] Design a small-run enclosure (3D printed)
- [ ] Finish the USB updater utility
- [ ] Close the battery gauge and set-time firmware items
- [ ] Review employment agreement for IP clauses
- [ ] Form an LLC before the first sale
- [ ] Build and sell the first 20 units
- [ ] Read the Crowd Supply and Kickstarter creator guidelines
- [ ] Look up local incubators and state small-business programs
