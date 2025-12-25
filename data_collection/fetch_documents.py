import gzip
import hashlib
import logging
import random
import re
import signal
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Tuple
from urllib.parse import urlparse, urlunparse
from pymongo import ReturnDocument
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse, urlunparse
import threading


import requests
import yaml
from pymongo import ASCENDING, MongoClient
from pymongo.collection import Collection

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
logger = logging.getLogger("crawler")

RUNNING = True


def _sig_handler(sig, frame):
    global RUNNING
    logger.info("Получен сигнал остановки. Завершусь после текущей итерации...")
    RUNNING = False


signal.signal(signal.SIGINT, _sig_handler)
signal.signal(signal.SIGTERM, _sig_handler)


def load_config(path: str) -> Dict[str, Any]:
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def normalize_url(url: str) -> str:
    p = urlparse(url)
    scheme = (p.scheme or "https").lower()
    netloc = (p.netloc or "").lower()
    path = p.path or "/"
    while "//" in path:
        path = path.replace("//", "/")
    fragment = ""
    query = p.query
    if path != "/":
        path = path.rstrip("/") or "/"
    return urlunparse((scheme, netloc, path, "", query, fragment))


def md5_text(s: str) -> str:
    return hashlib.md5(s.encode("utf-8", errors="ignore")).hexdigest()


def get_db(cfg: Dict[str, Any]):
    uri = cfg["db"]["uri"]
    dbname = cfg["db"].get("database", "searchlab")
    client = MongoClient(uri)
    db = client[dbname]

    db.urls.create_index([("url", ASCENDING)], unique=True)
    db.urls.create_index([("next_crawl_ts", ASCENDING), ("locked_until", ASCENDING)])

    db.docs.create_index([("url", ASCENDING)])
    db.docs.create_index([("crawl_ts", ASCENDING)])
    return db



def fetch(
    session: requests.Session,
    url: str,
    etag: Optional[str],
    last_modified: Optional[str],
    timeout: int,
    ua: str,
):
    headers = {
        "User-Agent": ua,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "en-US,en;q=0.9",
        "Connection": "keep-alive",
        "Cache-Control": "no-cache",
        "Pragma": "no-cache",
    }
    if etag:
        headers["If-None-Match"] = etag
    if last_modified:
        headers["If-Modified-Since"] = last_modified

    last_exc: Optional[Exception] = None

    for attempt in range(5):
        try:
            r = session.get(url, headers=headers, timeout=timeout, allow_redirects=True)

            if r.status_code == 406:
                time.sleep(2.0 * (attempt + 1))
                continue

            return r.status_code, {k.lower(): v for k, v in r.headers.items()}, r.text

        except requests.RequestException as e:
            last_exc = e
            time.sleep(1.0 * (attempt + 1))

    if last_exc:
        raise last_exc
    return 406, {}, ""



_BIB_URL_RE = re.compile(r'url\s*=\s*(?:\{([^}]+)\}|"([^"]+)")', re.IGNORECASE)

_BIB_YEAR_RE = re.compile(r"year\s*=\s*\{([^}]+)\}", re.IGNORECASE)


def iter_acl_urls_from_bib_gz(bib_gz_url: str, year_from: Optional[int], year_to: Optional[int]) -> Iterable[str]:
    resp = requests.get(bib_gz_url, timeout=60, headers={"User-Agent": "SearchLabBot/1.0"})
    resp.raise_for_status()
    data = gzip.decompress(resp.content).decode("utf-8", errors="ignore")

    cur_year: Optional[int] = None
    for line in data.splitlines():
        m_year = _BIB_YEAR_RE.search(line)
        if m_year:
            try:
                cur_year = int(m_year.group(1).strip())
            except Exception:
                cur_year = None

        m = _BIB_URL_RE.search(line)
        if not m:
            continue
        url = (m.group(1) or m.group(2) or "").strip()

        if not url.startswith("http"):
            continue
        if year_from is not None and cur_year is not None and cur_year < year_from:
            continue
        if year_to is not None and cur_year is not None and cur_year > year_to:
            continue
        yield url

@dataclass
class ArxivEntry:
    abs_url: str
    updated_year: Optional[int]


from datetime import datetime

def iter_arxiv_abs_urls(
    category: str,
    page_size: int,
    max_pages: int,
    year_from: Optional[int],
    year_to: Optional[int],
    ua: str,
):

    import xml.etree.ElementTree as ET
    import requests
    import time

    base = "https://export.arxiv.org/api/query"
    session = requests.Session()

    def do_query(search_query: str):
        for page in range(max_pages):
            start = page * page_size
            params = {
                "search_query": search_query,
                "start": start,
                "max_results": page_size,
                "sortBy": "submittedDate",
                "sortOrder": "descending",
            }

            last_err = None
            for _ in range(3):
                r = session.get(base, params=params, headers={"User-Agent": ua}, timeout=30)
                if r.status_code >= 500:
                    last_err = f"arXiv API {r.status_code} at start={start}"
                    time.sleep(2.0)
                    continue
                r.raise_for_status()
                last_err = None
                break

            if last_err is not None:
                logger.warning("arXiv API failed after retries: query=%s start=%d err=%s", search_query, start, last_err)
                break

            root = ET.fromstring(r.text)
            ns = {"a": "http://www.w3.org/2005/Atom"}
            entries = root.findall("a:entry", ns)

            logger.info("arXiv seed query=%s page=%d start=%d got=%d", search_query, page, start, len(entries))

            if not entries:
                break

            for e in entries:
                for lk in e.findall("a:link", ns):
                    if lk.attrib.get("rel") == "alternate":
                        href = lk.attrib.get("href", "")
                        if "/abs/" in href:
                            yield href.replace("http://", "https://")
                        break

    if year_from is not None and year_to is not None:
        for y in range(int(year_from), int(year_to) + 1):
            date_from = f"{y}01010000"
            date_to = f"{y}12312359"
            q = f"cat:{category} AND submittedDate:[{date_from} TO {date_to}]"
            yield from do_query(q)
    else:
        yield from do_query(f"cat:{category}")

def arxiv_fetch_url(url: str) -> str:
    p = urlparse(url)
    if p.netloc.lower() == "arxiv.org":
        return urlunparse((p.scheme or "https", "export.arxiv.org", p.path, "", p.query, ""))
    return url



class Crawler:
    def __init__(self, cfg: Dict[str, Any]):
        self.cfg = cfg
        self.db = get_db(cfg)
        self.urls: Collection = self.db.urls
        self.docs: Collection = self.db.docs

        logic = cfg.get("logic", {}) or {}

        self.workers = int(logic.get("workers", 1))
        self.lock_seconds = int(logic.get("lock_seconds", 120))

        self.seed_on_start = bool(logic.get("seed_on_start", True))
        self.seed_if_empty_only = bool(logic.get("seed_if_empty_only", True))

        delay = logic.get("delay_between_requests", [0.8, 1.6])
        if isinstance(delay, (int, float)):
            self.delay_min = self.delay_max = float(delay)
        else:
            self.delay_min = float(delay[0])
            self.delay_max = float(delay[1])

        self.revisit_interval = int(logic.get("revisit_interval", 7 * 86400))
        self.http_timeout = int(logic.get("http_timeout", 20))
        self.idle_sleep = int(logic.get("idle_sleep_seconds", 3))
        self.ua = str(logic.get("user_agent", "SearchLabBot/1.0 (+lab) email:your_email@example.com"))

        crawl = cfg.get("crawl", {}) or {}
        self.allowed_domains = set(d.lower() for d in (crawl.get("allowed_domains", []) or []))

        arxiv_min_interval = float(logic.get("arxiv_min_interval_seconds", 3.0))

        self._rate_lock = threading.Lock()
        self._next_allowed_ts: Dict[str, float] = {}

        self._min_interval_by_host = {
            "export.arxiv.org": arxiv_min_interval,
            "arxiv.org": arxiv_min_interval,
        }

    def _rate_limit(self, url: str):
        host = urlparse(url).netloc.lower()
        min_interval = self._min_interval_by_host.get(host)
        if not min_interval:
            return

        with self._rate_lock:
            now = time.time()
            next_allowed = self._next_allowed_ts.get(host, now)
            wait = next_allowed - now
            if wait > 0: 
                time.sleep(wait)
            self._next_allowed_ts[host] = time.time() + min_interval



    def _domain_ok(self, url: str) -> bool:
        if not self.allowed_domains:
            return True
        netloc = urlparse(url).netloc.lower()
        return netloc in self.allowed_domains
    
    def upsert_url(self, url: str, source: str, next_ts: int) -> bool:
        res = self.urls.update_one(
            {"url": url},
            {"$setOnInsert": {
                "url": url,
                "source": source,
                "etag": None,
                "last_modified": None,
                "hash": None,
                "last_crawl_ts": None,
                "status_code": None,
                "first_seen_ts": int(time.time()),
                "next_crawl_ts": int(next_ts),
            }},
            upsert=True,
        )
        return res.upserted_id is not None



    def claim_next_url(self) -> Optional[Dict[str, Any]]:
        now = int(time.time())

        rec = self.urls.find_one_and_update(
            {
                "next_crawl_ts": {"$lte": now},
                "$or": [{"locked_until": {"$exists": False}}, {"locked_until": {"$lte": now}}, {"locked_until": 0}],
            },
            {"$set": {"locked_until": now + self.lock_seconds}},
            sort=[("last_crawl_ts", ASCENDING), ("next_crawl_ts", ASCENDING)],
            return_document=ReturnDocument.AFTER,
        )

        return rec

    def seed(self):
        seeding = self.cfg.get("seeding", {}) or {}
        max_urls_per_source = int(seeding.get("max_urls_per_source", 15000))
        now = int(time.time())

        before = self.urls.count_documents({})
        logger.info("DB urls before seeding: %d", before)

        def add_url(raw_url: str, source: str) -> str:
            nu = normalize_url(raw_url)
            if not self._domain_ok(nu):
                return "skip"

            inserted = self.upsert_url(nu, source, now)
            return "inserted" if inserted else "exists"

        acl = seeding.get("acl", {}) or {}
        bib_gz_url = acl.get("bib_gz_url")
        year_from = acl.get("year_from")
        year_to = acl.get("year_to")

        if bib_gz_url:
            logger.info("Seeding ACL from %s", bib_gz_url)
            attempted = inserted_new = existed = skipped = 0

            for u in iter_acl_urls_from_bib_gz(bib_gz_url, year_from, year_to):
                if attempted >= max_urls_per_source:
                    break
                attempted += 1

                try:
                    st = add_url(u, "acl-anthology")
                except Exception:
                    logger.exception("Failed to upsert url: %s", u)
                    raise

                if st == "inserted":
                    inserted_new += 1
                elif st == "exists":
                    existed += 1
                else:
                    skipped += 1

            after_acl = self.urls.count_documents({})
            logger.info(
                "ACL seeding done: attempted=%d inserted_new=%d already_had=%d skipped_domain=%d",
                attempted, inserted_new, existed, skipped,
            )
            logger.info("DB urls after ACL: %d (+%d)", after_acl, after_acl - before)
            before = after_acl  

        arxiv = seeding.get("arxiv", {}) or {}
        category = str(arxiv.get("category", "cs.CL"))
        page_size = int(arxiv.get("page_size", 200))
        max_pages = int(arxiv.get("max_pages", 100))
        year_from = arxiv.get("year_from")
        year_to = arxiv.get("year_to")

        logger.info("Seeding arXiv via API: cat=%s", category)
        attempted = inserted_new = existed = skipped = 0

        for u in iter_arxiv_abs_urls(category, page_size, max_pages, year_from, year_to, ua=self.ua):
            if attempted >= max_urls_per_source:
                break
            attempted += 1

            try:
                st = add_url(u, "arxiv")
            except Exception:
                logger.exception("Failed to upsert url: %s", u)
                raise

            if st == "inserted":
                inserted_new += 1
            elif st == "exists":
                existed += 1
            else:
                skipped += 1

        after_arxiv = self.urls.count_documents({})
        logger.info(
            "arXiv seeding done: attempted=%d inserted_new=%d already_had=%d skipped_domain=%d",
            attempted, inserted_new, existed, skipped,
        )
        logger.info("DB urls after arXiv: %d (+%d)", after_arxiv, after_arxiv - before)


    def next_batch(self) -> List[Dict[str, Any]]:
        now = int(time.time())
        return list(
            self.urls.find({"next_crawl_ts": {"$lte": now}})
            .sort("next_crawl_ts", ASCENDING)
            .limit(self.batch_size)
        )

    def process_one(self, rec: Dict[str, Any], session: requests.Session):
        _id = rec["_id"]
        url = rec["url"]
        source = rec.get("source", urlparse(url).netloc)

        etag = rec.get("etag")
        last_modified = rec.get("last_modified")
        old_hash = rec.get("hash")

        fetch_url = arxiv_fetch_url(url)

        logger.info("GET %s (source=%s)", url, source)

        self._rate_limit(fetch_url)

        try:
            status, headers, body = fetch(session, fetch_url, etag, last_modified, self.http_timeout, self.ua)
        except requests.RequestException as e:
            logger.warning("Request error: %s", e)
            now_ts = int(time.time())
            self.urls.update_one(
                {"_id": _id},
                {
                    "$set": {
                        "last_crawl_ts": now_ts,
                        "next_crawl_ts": now_ts + 3600,
                        "status_code": None,
                    },
                    "$unset": {"locked_until": ""},
                },
            )
            return

        now_ts = int(time.time())

        if status == 304:
            self.urls.update_one(
                {"_id": _id},
                {
                    "$set": {
                        "last_crawl_ts": now_ts,
                        "next_crawl_ts": now_ts + self.revisit_interval,
                        "status_code": 304,
                    },
                    "$unset": {"locked_until": ""},
                },
            )
            return

        if status == 406:
            logger.warning("HTTP 406 for %s (will retry later)", url)
            self.urls.update_one(
                {"_id": _id},
                {
                    "$set": {
                        "last_crawl_ts": now_ts,
                        "next_crawl_ts": now_ts + 6 * 3600,
                        "status_code": 406,
                    },
                    "$unset": {"locked_until": ""},
                },
            )
            return

        if status == 200 and body:
            new_hash = md5_text(body)
            changed = (old_hash != new_hash)

            if changed:
                self.docs.insert_one({"url": url, "raw_html": body, "source": source, "crawl_ts": now_ts})
                logger.info("saved changed doc: %s", url)
            else:
                logger.info("unchanged (md5 same): %s", url)

            self.urls.update_one(
                {"_id": _id},
                {
                    "$set": {
                        "etag": headers.get("etag"),
                        "last_modified": headers.get("last-modified"),
                        "hash": new_hash,
                        "last_crawl_ts": now_ts,
                        "next_crawl_ts": now_ts + self.revisit_interval,
                        "status_code": 200,
                    },
                    "$unset": {"locked_until": ""},
                },
            )
            return

        logger.warning("HTTP %s for %s", status, url)
        backoff = 24 * 3600
        if 500 <= status < 600:
            backoff = 2 * 3600

        self.urls.update_one(
            {"_id": _id},
            {
                "$set": {
                    "last_crawl_ts": now_ts,
                    "next_crawl_ts": now_ts + backoff,
                    "status_code": status,
                },
                "$unset": {"locked_until": ""},
            },
        )

    def run(self):
        logger.info("Start crawler")

        if self.seed_on_start:
            if self.seed_if_empty_only:
                cnt = self.urls.estimated_document_count()
                if cnt == 0:
                    self.seed()
                else:
                    logger.info("Skip seeding (urls not empty): %d", cnt)
            else:
                self.seed()

        def worker_loop(wid: int):
            logger.info("worker-%d started", wid)
            session = requests.Session()

            while RUNNING:
                rec = self.claim_next_url()
                if not rec:
                    time.sleep(self.idle_sleep)
                    continue

                self.process_one(rec, session=session)
                time.sleep(random.uniform(self.delay_min, self.delay_max))

            logger.info("worker-%d stopped", wid)

        if self.workers <= 1:
            worker_loop(0)
        else:
            with ThreadPoolExecutor(max_workers=self.workers) as ex:
                futures = [ex.submit(worker_loop, i) for i in range(self.workers)]
                for f in futures:
                    f.result()

        logger.info("Crawler stopped")




def main():
    if len(sys.argv) != 2:
        print("Usage: python crawler.py config.yaml")
        sys.exit(1)
    cfg = load_config(sys.argv[1])
    Crawler(cfg).run()


if __name__ == "__main__":
    main()
