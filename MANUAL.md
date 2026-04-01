# SERP Analyzer & Prompt Builder - User Manual

> **Version:** 1.0
> **Platform:** Windows 10/11

---

## Table of Contents

1. [What Is This Tool?](#1-what-is-this-tool)
2. [Installation & First Launch](#2-installation--first-launch)
3. [Initial Setup (Settings Tab)](#3-initial-setup-settings-tab)
   - [Adding API Keys](#31-adding-serper-api-keys)
   - [Adding Proxies](#32-adding-proxies-optional)
   - [Configuring Threads & HTTP Settings](#33-configuring-threads--http-settings)
   - [Downloading NLP Models](#34-downloading-nlp-models-optional)
4. [Running Your First Analysis (Search Tab)](#4-running-your-first-analysis-search-tab)
   - [Single Keyword Search](#41-single-keyword-search)
   - [Batch Keyword Search](#42-batch-keyword-search)
   - [Understanding the Search Options](#43-understanding-the-search-options)
   - [The Pending Queue](#44-the-pending-queue)
5. [Reviewing Scraped Pages (Results Tab)](#5-reviewing-scraped-pages-results-tab)
   - [The Results List](#51-the-results-list)
   - [Right-Click Actions](#52-right-click-actions)
6. [Working with Prompts & Analysis (Analysis Tab)](#6-working-with-prompts--analysis-analysis-tab)
   - [The Generated Prompt](#61-the-generated-prompt)
   - [Choosing a Template](#62-choosing-a-template)
   - [Prompt Options (Checkboxes & Caps)](#63-prompt-options-checkboxes--caps)
   - [NLP Info Panel](#64-nlp-info-panel)
   - [Exporting Your Prompt](#65-exporting-your-prompt)
   - [Content Gap Analysis](#66-content-gap-analysis)
7. [Session History](#7-session-history)
8. [Monitoring Resources](#8-monitoring-resources)
9. [The Log Tab](#9-the-log-tab)
10. [Data Management](#10-data-management)
11. [Tips & Best Practices](#11-tips--best-practices)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. What Is This Tool?

SERP Analyzer & Prompt Builder is a desktop application for internet marketers, SEO professionals, and content creators. It answers one question: **"Why do the top Google results rank for this keyword, and how do I write content that competes?"**

Here's what it does, step by step:

1. **Searches Google** for your target keyword using the Serper API
2. **Scrapes the top-ranking pages** to extract their full text, headings, links, and structure
3. **Runs NLP analysis** on the scraped content -- extracting keywords, entities, heading patterns, search intent, and competitive statistics
4. **Generates an optimized system prompt** you can paste directly into ChatGPT, Claude, or any AI writing tool

The result is a data-backed writing brief that tells an AI writer exactly what keywords to use, what headings to include, what word count to target, and what structure to follow -- all based on what Google is actually ranking right now.

<!-- SCREENSHOT: Main application window overview showing the dark theme and tab layout -->

---

## 2. Installation & First Launch

### Requirements

- Windows 10 or later
- Internet connection (for API searches and web scraping)
- A free Serper API key (see Section 3.1)
- Approximately 100 MB disk space (plus ~1.5 GB if you download the optional NLP models)

### Launching the Application

1. Navigate to the application folder
2. Run **serp_to_prompt_writer.exe**
3. The application opens to the **Search** tab

On first launch, the app automatically:
- Creates the `data/`, `output/`, and `models/` directories
- Creates a fresh SQLite database for storing your sessions
- Detects your CPU, RAM, and GPU capabilities
- Sets default thread counts based on your hardware

<!-- SCREENSHOT: Application on first launch, empty state -->

---

## 3. Initial Setup (Settings Tab)

Click the **Settings** tab to configure the application before your first analysis.

<!-- SCREENSHOT: Settings tab overview -->

### 3.1 Adding Serper API Keys

You need at least one Serper API key to search Google. The free tier gives you **100 searches per day**.

**Getting a key:**
1. Go to [serper.dev](https://serper.dev) and create a free account
2. Copy your API key from the dashboard

**Adding it to the app:**
1. On the Settings tab, find the **API Keys** section
2. Paste your key into the input field
3. Click **Add**
4. The key appears in the keys list

You can add **multiple keys** for higher daily limits. The app rotates through them automatically (round-robin), distributing searches evenly.

**Checking credits:** Right-click any key in the list and select **Check Credits** to see how many searches remain on that key today.

<!-- SCREENSHOT: API Keys section with a key added, showing the list and Add button -->
<!-- SCREENSHOT: Right-click context menu on a key showing "Check Credits" -->

### 3.2 Adding Proxies (Optional)

Private proxies let you scrape web pages without getting rate-limited or blocked. Each proxy is a separate IP address that the app rotates through.

1. Find the **Proxies** section on the Settings tab
2. Enter a proxy URL in the format `http://host:port` or `socks5://host:port`
3. Click **Add**
4. If your proxies require authentication, fill in the **Username** and **Password** fields -- these credentials apply to all proxies in the list

**Auto-Map Proxies:** Click this button to automatically pair your API keys with proxies. This distributes API traffic across different IPs so no single proxy handles all requests.

Proxies are optional. Without them, all scraping comes from your machine's IP address, which works fine for moderate use.

<!-- SCREENSHOT: Proxy section with a few proxies added, showing username/password fields -->

### 3.3 Configuring Threads & HTTP Settings

**Threads:** Controls how many pages are scraped simultaneously. The default is set to twice your CPU core count, which is a good starting point.

| Setting | What it does | Default |
|---------|-------------|---------|
| **Threads** | Concurrent scrape workers (2 to 256) | CPU cores x 2 |
| **GET Timeout** | Seconds to wait for a page to respond | 30s |
| **POST Timeout** | Seconds to wait for API responses | 30s |
| **Max Redirects** | How many redirects to follow before giving up | 5 |
| **Retry Count** | How many times to retry a failed request | 3 |
| **Retry Base (ms)** | Starting delay between retries (doubles each time) | 1000ms |

The retry system uses **exponential backoff**: if the base is 1000ms, retries happen at 1s, 2s, 4s, etc. This prevents hammering a slow server.

If the app detects your system may be underpowered for the thread count you've selected, it will display a warning.

<!-- SCREENSHOT: Threading and HTTP settings section -->

### 3.4 Downloading NLP Models (Optional)

The application can use machine learning models for more accurate analysis. Without them, the app still works using heuristic-based analysis -- but with them, you get:

- **Named Entity Recognition (NER):** Identifies people, organizations, locations, and products mentioned across competitor pages
- **Semantic Similarity:** Measures how closely each keyword relates to your target topic using BERT embeddings
- **Content Type Classification:** Automatically detects whether the SERP favors how-tos, comparisons, listicles, reviews, etc.

**To download:**
1. Click **Download Models** on the Settings tab
2. The download is approximately **1.5 GB total** (three models)
3. A progress indicator shows the download status
4. Models are saved to the `models/` folder and only need to be downloaded once

The models are:
| Model | Size | Purpose |
|-------|------|---------|
| bert-base-NER | ~400 MB | Named entity extraction |
| all-MiniLM-L6-v2 | ~80 MB | Sentence embeddings for semantic scoring |
| distilbart-mnli | ~978 MB | Zero-shot content type classification |

If you have an NVIDIA GPU, the models will use it automatically for faster inference. Otherwise, they run on your CPU.

<!-- SCREENSHOT: Download Models button and system info display showing model status -->

---

## 4. Running Your First Analysis (Search Tab)

The Search tab is your starting point for every analysis.

<!-- SCREENSHOT: Search tab with all controls visible -->

### 4.1 Single Keyword Search

1. Type your target keyword into the **keyword input box** (e.g., `best running shoes 2026`)
2. Set your desired options (see Section 4.3)
3. Click **Go**

The app then:
1. Queries the Serper API for Google's top results
2. Filters out non-competitive domains (Wikipedia, Amazon, Reddit, etc.)
3. Scrapes each competitor page for its full content
4. Runs NLP analysis on the aggregated content
5. Generates an optimized writing prompt
6. Saves the session to your history

The **progress bar** and **status message** at the bottom show you what's happening in real time. Pages appear in the Results tab as they're scraped.

To **cancel** a running analysis at any time, click the **Stop** button.

<!-- SCREENSHOT: Analysis in progress showing progress bar and status message -->

### 4.2 Batch Keyword Search

To analyze multiple keywords at once, enter them **one per line** or **separated by commas**:

```
best running shoes 2026
trail running shoes for beginners
running shoe reviews
```

Click **Go** and the app processes each keyword sequentially. Each keyword gets its own full analysis pipeline and appears as a separate entry in your session history.

<!-- SCREENSHOT: Keyword input box with multiple keywords entered -->

### 4.3 Understanding the Search Options

Below the keyword input, you'll find several controls that affect how the analysis runs:

#### Pages

How many pages of Google results to fetch per keyword. Each page contains ~10 results.

| Pages | Total URLs analyzed |
|-------|-------------------|
| 1 | ~10 |
| 2 | ~20 |
| 3 | ~30 |
| 4 | ~40 |
| 5 | ~50 |

More pages means more data but longer analysis time. **1-2 pages is usually sufficient** for most keywords.

#### Depth

Controls how deep the app digs into outbound links found on the scraped pages. Setting this higher means the app follows external links on the SERP results to discover additional related content.

#### Checkboxes

| Checkbox | What it does |
|----------|-------------|
| **Auto-Scrape** | Automatically scrape all URLs from the Serper results (recommended -- leave on) |
| **Auto-NLP** | Automatically run NLP analysis after scraping finishes (recommended -- leave on) |
| **Auto OBL** | Automatically crawl outbound links found on scraped pages for additional keyword data |
| **Use Proxies** | Route page scraping through your configured proxy pool |

**Recommended defaults for most users:** Auto-Scrape ON, Auto-NLP ON, Auto OBL OFF, Use Proxies OFF.

Turn on **Auto OBL** when you want a deeper topical analysis -- the app will follow links from the SERP results to discover additional related content and keywords that competitors are referencing.

<!-- SCREENSHOT: Close-up of the Pages/Depth combos and checkboxes -->

### 4.4 The Pending Queue

When the Serper API returns results, URLs appear in the **Pending Queue** below the search controls. If Auto-Scrape is ON, they move to scraping automatically. If Auto-Scrape is OFF, you can manually choose which pages to process.

**Right-click the Pending Queue** for options:

| Option | What it does |
|--------|-------------|
| **View Serper Details** | Shows the title, URL, snippet, and position from Google's results |
| **Scrape Selected** | Fetches and parses the selected URLs |
| **Build Prompt from Selected** | Runs NLP + prompt generation on just the selected pages |
| **Scrape All** | Scrapes every URL in the queue |
| **Build Prompt from All** | Runs the full pipeline on all queued pages |
| **Delete Selected / Delete All** | Removes URLs from the queue |

This gives you fine-grained control: you can review Serper's results, cherry-pick the most relevant competitor pages, and exclude ones that aren't useful.

<!-- SCREENSHOT: Pending queue with URLs listed and right-click context menu open -->

---

## 5. Reviewing Scraped Pages (Results Tab)

After scraping completes, switch to the **Results** tab to inspect what was collected.

<!-- SCREENSHOT: Results tab with a populated results list -->

### 5.1 The Results List

Each row in the results list represents one scraped page, showing:

- **Domain** of the page
- **Title** of the page
- **Word Count** of the extracted body text
- **Heading Count** (H2/H3 tags found)
- **Scrape Status** (success, JS fallback used, or failed)

Click any row to see the full extracted content in the detail panel on the right.

Pages that required JavaScript rendering (because the initial text-only fetch returned too little content) are marked with a **JS fallback** indicator. This happens automatically -- the app first tries a fast text-only request, and if the result looks thin (under ~200 words or showing JS framework signals), it retries with a headless browser.

<!-- SCREENSHOT: Results list with a page selected, showing detail panel -->

### 5.2 Right-Click Actions

Right-click the results list for additional options:

| Option | What it does |
|--------|-------------|
| **Build Prompt from Selected** | Run NLP analysis and generate a prompt using only the selected pages |
| **Crawl OBL for Selected** | Follow outbound links on the selected pages to discover more content |
| **Delete Selected** | Remove pages from results |
| **Select All** | Select every page in the list |

The **Build Prompt from Selected** option is useful when you want to exclude low-quality pages from your analysis. For example, if a scraped page has very low word count or is clearly not a real competitor, deselect it and build your prompt from just the strong pages.

<!-- SCREENSHOT: Results list right-click context menu -->

---

## 6. Working with Prompts & Analysis (Analysis Tab)

The Analysis tab is where you see the results of the NLP analysis and the generated prompt. This is the main output of the tool.

<!-- SCREENSHOT: Full Analysis tab overview -->

### 6.1 The Generated Prompt

The large text area on the left displays the **generated system prompt**. This is what you copy and paste into your AI writer. A typical prompt includes:

- **Content Guidelines** -- the detected search intent (informational, commercial, transactional, navigational), content type, and recommended tone
- **Target Audience** -- who the content is for, adapted to the intent
- **Output Format** -- markdown structure recommendations, specific to the content type
- **Semantic Keywords** -- primary and secondary keywords with confidence scores, extracted from competitor analysis
- **Named Entities** -- people, organizations, products, and locations to reference for topical authority
- **Recommended Outline** -- H2/H3 heading structure based on what top competitors are using
- **Competitive Metrics** -- average word count, heading density, and link patterns across the SERP
- **People Also Ask** -- questions from Google's PAA box to answer in your content
- **Related Searches** -- additional subtopics Google associates with this keyword
- **Writing Instructions** -- numbered step-by-step writing directions
- **Reader Goal** -- what the reader should accomplish after reading
- **Linking Guidance** -- recommended internal and external link counts

<!-- SCREENSHOT: Generated prompt text showing the different sections -->

### 6.2 Choosing a Template

Use the **Template** dropdown to switch between four prompt formats:

#### Full System Prompt
The complete prompt with all sections. Best for feeding into AI writers that need comprehensive instructions. This is the default and recommended template for most use cases.

#### Keywords Only
A tabular format focused on keyword data: primary keywords with scores and frequency, secondary keywords, entities, and related searches. Best when you already have your article structure and just need keyword guidance.

#### Outline Only
Focuses on the article structure: introduction, H2/H3 headings from competitors, conclusion, and FAQ section from PAA questions. Best when you need a content outline to hand to a human writer.

#### Competitive Brief
An executive-level summary: SERP landscape, benchmarks (word counts, heading counts), and minimum requirements to be competitive. Best for content strategists planning what to produce.

All four templates include shared sections (target audience, output format, writing instructions, reader goal, and linking guidance) so every output is actionable.

<!-- SCREENSHOT: Template dropdown showing the four options -->

### 6.3 Prompt Options (Checkboxes & Caps)

Below the template selector, you'll find checkboxes to toggle individual sections of the prompt on or off:

| Checkbox | Section it controls |
|----------|-------------------|
| **Keywords** | Primary and secondary keyword lists |
| **Entities** | Named entities (people, orgs, locations, products) |
| **Outline** | Recommended H2/H3 heading structure |
| **Stats** | Competitive content statistics |
| **PAA** | People Also Ask questions |
| **Related** | Related search queries |

Use the **Max Keywords** and **Max Entities** fields to cap how many of each appear in the prompt. This is useful when you want a more focused prompt -- set Max Keywords to 15 instead of showing all 50+ extracted keywords.

When set to 0, the defaults are used (20 primary keywords, 15 secondary).

<!-- SCREENSHOT: Prompt option checkboxes and max keyword/entity fields -->

### 6.4 NLP Info Panel

On the right side of the Analysis tab, the **NLP Info** panel shows the raw analysis data in a scrollable list. This is the full breakdown of everything the NLP engine discovered:

- **Top keywords** with their three-signal scores (TF-IDF, corpus frequency, semantic similarity)
- **Entity list** with labels (PER, ORG, LOC, PRODUCT) and frequency counts
- **Heading patterns** seen across competitors with occurrence counts
- **Search intent classification** and confidence
- **Content type detection** (how-to, guide, comparison, review, listicle, recipe, etc.)
- **Content statistics** (average/min/max word counts, heading ratios)
- **PAA questions** and their snippets
- **Related searches**

You can **multi-select** items and **right-click** to copy them:

| Option | What it copies |
|--------|---------------|
| **Copy Selected (text only)** | Just the keyword/entity/heading text |
| **Copy Selected (with scores)** | Text plus the score, frequency, and label data |
| **Select All** | Selects every item in the list |

<!-- SCREENSHOT: NLP Info panel showing keywords with scores -->

### 6.5 Exporting Your Prompt

Four export buttons sit below the prompt text area:

| Button | What it does |
|--------|-------------|
| **Copy** | Copies the prompt to your clipboard, ready to paste into ChatGPT/Claude |
| **Save TXT** | Saves as a plain text file to the `output/` folder |
| **Save JSON** | Saves as structured JSON (keywords, entities, headings, stats -- all as separate data fields). Ideal for feeding into automation scripts or API pipelines |
| **Save MD** | Saves as a formatted Markdown report |

Files are automatically named based on the keyword, e.g., `best running shoes 2026.txt`.

<!-- SCREENSHOT: Export buttons row -->

### 6.6 Content Gap Analysis

At the bottom of the Analysis tab is the **Content Gap Analyzer**. This compares your existing content against what the top competitors are doing.

**How to use it:**
1. Paste your existing article text (or a draft) into the **Content Gap input** box
2. Click **Analyze Gap**
3. The results show:
   - **Missing keywords** that competitors use but your content doesn't
   - **Missing headings** that top pages include but yours doesn't
   - **Word count gap** -- how your content length compares to the SERP average
   - **Coverage percentage** -- what fraction of the top keyword/heading signals your content already covers

This is invaluable for updating existing content. Instead of rewriting from scratch, you can see exactly what's missing and surgically add it.

<!-- SCREENSHOT: Content Gap Analyzer with sample text pasted and gap results showing -->

---

## 7. Session History

Every analysis you run is automatically saved. The **History** panel on the Search tab (left side) lists all your previous sessions.

- **Click** a session to load its analysis data into the Analysis tab
- **Double-click** a session to load it and switch to the Analysis tab immediately
- **Right-click** for options:
  - **Delete** -- remove the selected session
  - **Delete All** -- clear all history

Each history entry shows enriched metadata: the keyword, detected intent, content type, number of keywords/entities extracted, and the date of analysis.

Sessions are stored in the SQLite database (`data/serp_analyzer.db`) and persist across application restarts.

<!-- SCREENSHOT: History list with several sessions, showing the metadata format -->

---

## 8. Monitoring Resources

The bottom-right corner of the application displays a real-time **resource monitor**:

- **CPU %** -- your processor usage
- **RAM %** -- memory consumption
- **GPU %** -- GPU utilization (if you have an NVIDIA GPU and the NLP models are loaded)

These indicators are color-coded:
- **Green**: 0-70% (normal)
- **Yellow**: 70-90% (elevated)
- **Red**: 90%+ (heavy load)

When scraping many pages concurrently with high thread counts, keep an eye on these. If CPU or RAM goes red, consider reducing your thread count in Settings.

<!-- SCREENSHOT: Resource monitor showing CPU/RAM/GPU percentages -->

---

## 9. The Log Tab

The Log tab provides a real-time, detailed log of everything the application is doing. Each entry has a timestamp, severity level, and message.

**Log levels:**
| Level | Color | What it shows |
|-------|-------|--------------|
| **DBG** | Gray | Debug-level detail (HTTP requests, parsing steps) |
| **INFO** | Cyan | Normal operations (search started, page scraped, NLP complete) |
| **WARN** | Yellow | Non-fatal issues (slow response, fallback to JS render) |
| **ERR** | Red | Failures (page couldn't be scraped, API error) |

Use the **filter buttons** at the top to toggle each level. Debug is off by default -- turn it on when troubleshooting. Click **Clear** to reset the log.

The log holds up to 2,000 entries in a ring buffer (oldest entries are dropped when full). Logs are also written to `debug.log` in the application folder, with automatic file rotation at 5 MB.

<!-- SCREENSHOT: Log tab showing mixed log levels with color coding -->

---

## 10. Data Management

On the Settings tab, the **Data & Tools** section provides maintenance options:

| Button | What it does |
|--------|-------------|
| **Download Models** | Downloads the NLP machine learning models (~1.5 GB) |
| **Purge Database** | Deletes all session history, scraped results, and pending URLs. **Your settings (API keys, proxies, thread config) are preserved** -- they're stored in a separate database |

The **System Info** display shows:
- CPU core count
- Available RAM
- GPU name(s) and VRAM (if detected)
- NLP model installation status (e.g., "NER: installed", "NLI: missing")

<!-- SCREENSHOT: Data & Tools section showing system info and model status -->

---

## 11. Tips & Best Practices

### Getting the Best Results

- **Start with 1 page of results.** 10 competitor pages usually gives you enough keyword and heading data. Add more pages only for very competitive niches.
- **Leave Auto-Scrape and Auto-NLP on.** The automated pipeline handles everything. Turn them off only when you want manual control over which pages to include.
- **Download the NLP models.** The heuristic-only mode works, but entity extraction and semantic scoring significantly improve keyword quality.
- **Use the Full System Prompt template first.** It gives AI writers the most context. Switch to other templates once you're familiar with what each section does.

### Working with AI Writers

1. Run an analysis for your target keyword
2. Click **Copy** on the Analysis tab
3. Open ChatGPT, Claude, or your preferred AI tool
4. Paste the prompt as a **system message** or as the first user message
5. Follow up with: "Write the article following the instructions above."

The prompt is designed to work as-is. The AI writer receives the exact keyword targets, heading structure, word count goals, and formatting rules that match what Google is currently ranking.

### Content Updates

Use the **Content Gap Analyzer** to audit existing content:
1. Analyze the keyword your existing article targets
2. Paste your current article into the gap analyzer
3. Review missing keywords and headings
4. Update your article to fill the gaps
5. Re-analyze to verify coverage improvement

### Batch Workflows

For content planning across a topic cluster:
1. Enter 5-10 related keywords (one per line)
2. Run the batch analysis
3. Review each session in History
4. Export all prompts as JSON for use in automation pipelines
5. The JSON format includes structured keyword data that scripts can parse programmatically

### Proxy Usage

- Use proxies when scraping more than ~50 pages in a session to avoid IP-based rate limiting
- The app automatically tracks proxy success rates and disables proxies that fall below 20% success after 10 attempts
- SOCKS5, HTTP, and HTTPS proxy protocols are all supported

---

## 12. Troubleshooting

### "No API key configured"

You haven't added a Serper API key. Go to Settings and add one (see Section 3.1).

### Search returns no results

- Check that your API key has remaining credits (right-click the key, select **Check Credits**)
- Verify your internet connection
- Check the Log tab for error details

### Pages show very low word counts

Some websites use heavy JavaScript rendering. The app automatically retries these with a headless browser, but some sites may still block scraping. Check the Log tab for JS fallback messages. Low word count pages can be excluded from your analysis by deselecting them on the Results tab.

### NLP analysis seems basic (no entities shown)

The NLP models haven't been downloaded. Go to Settings and click **Download Models**. Without models, the app still runs TF-IDF and n-gram analysis but can't do entity extraction or semantic scoring.

### Application feels slow during scraping

- Reduce thread count in Settings
- Check the resource monitor -- if CPU/RAM is in the red, you're overloading your system
- If using proxies, slow proxies can bottleneck the entire pipeline

### Database seems corrupted

Click **Purge Database** in Settings. This clears all session data but preserves your settings. The database is recreated fresh on next use.

### Models failed to download

- Check your internet connection
- Check disk space (models need ~1.5 GB)
- Try again -- the download resumes where it left off
- Check the Log tab for specific error messages

---

## Filtered Domains

The app automatically filters out non-competitive domains from SERP results. These are sites that rank due to domain authority rather than content quality, and aren't useful for competitive analysis:

- **Encyclopedias:** Wikipedia, Britannica, Merriam-Webster
- **Marketplaces:** Amazon, eBay, Walmart, Etsy, Best Buy, etc.
- **Social media:** Facebook, Twitter/X, Instagram, Reddit, Pinterest, LinkedIn, etc.
- **Video platforms:** YouTube, Vimeo, TikTok
- **News outlets:** CNN, BBC, NYT, Reuters, etc.
- **Tech giants:** Google, Microsoft, Apple, Adobe properties
- **Government sites:** All .gov domains

The blocklist is stored in `data/blocklist.txt` and can be edited manually if you want to add or remove domains.

---

## Keyboard Shortcuts & Quick Actions

| Action | How |
|--------|-----|
| Start analysis | Click **Go** or press Enter in the keyword box |
| Cancel analysis | Click **Stop** |
| Load history session | Click a session in the History list |
| Jump to Analysis tab | Double-click a History session |
| Copy prompt | Click **Copy** on the Analysis tab |
| Select all in a list | Right-click, **Select All** |
| Copy list items | Right-click selected items, choose text-only or with-scores |

---

*For bug reports or feature requests, please contact the developer.*
