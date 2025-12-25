SHELL := /bin/bash
.ONESHELL:
.SHELLFLAGS := -euo pipefail -c
.DEFAULT_GOAL := help

ROOT := $(CURDIR)
OUT_DIR ?= ./corpus_out
DOCS_DIR := $(OUT_DIR)/docs

CPP_DIR := cpp/src
BIN_DIR := $(CPP_DIR)/bin

DATA_DIR := data_collection
PREP_DIR := preprocessing
ANALYSIS_DIR := analysis

CFG ?= $(firstword $(wildcard $(DATA_DIR)/config.yaml config.yaml))

FETCHER   := $(firstword $(wildcard $(DATA_DIR)/fetch_documents.py))
DUMPER    := $(firstword $(wildcard $(DATA_DIR)/dump_corpus.py))
MONITOR   := $(firstword $(wildcard $(DATA_DIR)/fetch_monitor.py))
REQS      := $(firstword $(wildcard $(DATA_DIR)/requirements.txt requirements.txt))

STEMMING ?= 1
CHUNK ?= 2000000
CHUNK_PAIRS ?= 2000000
LIMIT ?= 10

DOCS_LIST := $(OUT_DIR)/docs_list.txt
DOCS_LIST_ABS := $(OUT_DIR)/docs_list_abs.txt
META_TSV := $(OUT_DIR)/meta.tsv
META_DOCID := $(OUT_DIR)/meta_docid.tsv

TOKENIZE_MARK := $(OUT_DIR)/.tokenize_ready
ACTIVE_STEM_FILE := $(OUT_DIR)/.active_stemming

VENV_PY := .venv/bin/python
VENV_PIP := .venv/bin/pip
DEPS_MARK := .venv/.deps_installed

TOKEN_STATS_BIN := $(BIN_DIR)/text_token_stats
TERM_FREQ_BIN := $(BIN_DIR)/term_frequency
BOOL_INDEX_BIN := $(BIN_DIR)/boolean_index_builder
BOOL_SEARCH_BIN := $(BIN_DIR)/boolean_search_cli

.PHONY: help install deps download monitor tokenize zipf index search full \
        build_cpp require_tokenize check_scripts \
        termfreq zipf_plot bool_index bool_query \
        clean clean_index

help:
	@echo "Команды:"
	@echo "  make install                  - установка зависимостей"
	@echo "  make download CFG=...         - сбор документов"
	@echo "  make monitor  CFG=...         - мониторинг сбора"
	@echo "  make tokenize STEMMING=0|1    - предобработка и токенизация"
	@echo "  make zipf                     - частоты и закон Ципфа"
	@echo "  make index                    - построение булевого индекса"
	@echo "  make search Q='...'           - булев поиск"
	@echo "  make full                     - полный пайплайн"
	@echo ""
	@echo "Активный режим стемминга: $(ACTIVE_STEM_FILE)"
	@echo "OUT_DIR = $(OUT_DIR)"

install: deps
deps: $(DEPS_MARK)

.venv/bin/python:
	python3 -m venv .venv

$(DEPS_MARK): .venv/bin/python
	@if [ -z "$(REQS)" ]; then echo "ERROR: requirements.txt не найден" && exit 2; fi
	$(VENV_PIP) install -r "$(REQS)"
	$(VENV_PIP) install -q matplotlib
	@touch "$(DEPS_MARK)"
	@echo "OK: deps installed"

download: deps
	@if [ -z "$(FETCHER)" ]; then echo "ERROR: fetch_documents.py не найден" && exit 2; fi
	@CFG_PATH="$(CFG)"; \
	if [ -d "$$CFG_PATH" ]; then CFG_PATH="$$CFG_PATH/config.yaml"; fi; \
	if [ ! -f "$$CFG_PATH" ]; then echo "ERROR: config file not found: $$CFG_PATH" && exit 2; fi; \
	$(VENV_PY) "$(FETCHER)" "$$CFG_PATH"

monitor: deps
	@if [ -z "$(MONITOR)" ]; then echo "ERROR: fetch_monitor.py не найден" && exit 2; fi
	@CFG_PATH="$(CFG)"; \
	if [ -d "$$CFG_PATH" ]; then CFG_PATH="$$CFG_PATH/config.yaml"; fi; \
	if [ ! -f "$$CFG_PATH" ]; then echo "ERROR: config file not found: $$CFG_PATH" && exit 2; fi; \
	$(VENV_PY) "$(MONITOR)" "$$CFG_PATH"

check_scripts:
	@test -f "$(PREP_DIR)/build_doc_lists.py" || (echo "ERROR: missing build_doc_lists.py" && exit 2)
	@test -f "$(PREP_DIR)/assign_doc_ids.py" || (echo "ERROR: missing assign_doc_ids.py" && exit 2)
	@test -f "$(ANALYSIS_DIR)/zipf_analysis.py" || (echo "ERROR: missing zipf_analysis.py" && exit 2)
	@echo "OK: scripts exist"

tokenize: deps check_scripts build_cpp
	@if [ -z "$(DUMPER)" ]; then echo "ERROR: dump_corpus.py не найден" && exit 2; fi
	@CFG_PATH="$(CFG)"; \
	if [ -d "$$CFG_PATH" ]; then CFG_PATH="$$CFG_PATH/config.yaml"; fi; \
	if [ ! -f "$$CFG_PATH" ]; then echo "ERROR: config file not found: $$CFG_PATH" && exit 2; fi; \
	mkdir -p "$(OUT_DIR)"
	@echo "[1/4] dump corpus -> $(OUT_DIR)"
	$(VENV_PY) "$(DUMPER)" "$$CFG_PATH"
	@test -f "$(META_TSV)" || (echo "ERROR: $(META_TSV) не создан" && exit 2)
	@test -d "$(DOCS_DIR)" || (echo "ERROR: $(DOCS_DIR) не создан" && exit 2)

	@echo "[2/4] build docs lists"
	$(VENV_PY) "$(PREP_DIR)/build_doc_lists.py" \
	  --docs_dir "$(DOCS_DIR)" \
	  --out_list "$(DOCS_LIST)" \
	  --out_abs "$(DOCS_LIST_ABS)"

	@echo "[3/4] assign doc ids"
	$(VENV_PY) "$(PREP_DIR)/assign_doc_ids.py" \
	  --meta_in "$(META_TSV)" \
	  --docs_list_abs "$(DOCS_LIST_ABS)" \
	  --meta_out "$(META_DOCID)"

	@echo "[4/4] tokenization stats (stemming=$(STEMMING))"
	"$(TOKEN_STATS_BIN)" "$(DOCS_LIST_ABS)" --stemming "$(STEMMING)" > "$(OUT_DIR)/token_stats_s$(STEMMING).txt"
	@test -f "$(OUT_DIR)/token_stats_s$(STEMMING).txt" || (echo "ERROR: token_stats not created" && exit 2)

	@echo "$(STEMMING)" > "$(ACTIVE_STEM_FILE)"
	@touch "$(TOKENIZE_MARK)"
	@echo "OK: tokenize done"

require_tokenize:
	@if [ ! -f "$(TOKENIZE_MARK)" ]; then echo "ERROR: tokenize not done" && exit 2; fi
	@test -f "$(ACTIVE_STEM_FILE)" || (echo "ERROR: no active stemming file" && exit 2)
	@test -f "$(DOCS_LIST_ABS)" || (echo "ERROR: no docs list" && exit 2)
	@test -f "$(META_DOCID)" || (echo "ERROR: no meta_docid" && exit 2)

zipf: require_tokenize termfreq zipf_plot

termfreq: require_tokenize build_cpp
	@S=$$(cat "$(ACTIVE_STEM_FILE)"); \
	OUT="$(OUT_DIR)/termfreq_s$$S.tsv"; LOG="$(OUT_DIR)/termfreq_s$$S.log"; \
	"$(TERM_FREQ_BIN)" "$(DOCS_LIST_ABS)" "$$OUT" --stemming "$$S" --chunk "$(CHUNK)" 2> "$$LOG"; \
	echo "OK: wrote $$OUT"

zipf_plot: require_tokenize
	@S=$$(cat "$(ACTIVE_STEM_FILE)"); \
	TF="$(OUT_DIR)/termfreq_s$$S.tsv"; \
	CSV="$(OUT_DIR)/zipf_s$$S.csv"; \
	PNG="$(OUT_DIR)/zipf_s$$S.png"; \
	test -f "$$TF" || (echo "ERROR: no termfreq" && exit 2); \
	$(VENV_PY) "$(ANALYSIS_DIR)/zipf_analysis.py" --termfreq "$$TF" --out_csv "$$CSV" --out_png "$$PNG"

index: require_tokenize bool_index

bool_index: require_tokenize build_cpp
	@S=$$(cat "$(ACTIVE_STEM_FILE)"); \
	DIR="$(OUT_DIR)/boolean_index_s$$S"; \
	mkdir -p "$$DIR"; \
	"$(BOOL_INDEX_BIN)" "$(DOCS_LIST_ABS)" "$(META_DOCID)" "$$DIR" --stemming "$$S" --chunk_pairs "$(CHUNK_PAIRS)"

search: require_tokenize bool_query

bool_query: require_tokenize build_cpp
	@S=$$(cat "$(ACTIVE_STEM_FILE)"); \
	DIR="$(OUT_DIR)/boolean_index_s$$S"; \
	if [ ! -f "$$DIR/terms.bin" ]; then echo "ERROR: index not found" && exit 2; fi; \
	if [ -z "$(strip $(Q))" ]; then echo "ERROR: empty query" && exit 2; fi; \
	set +H; \
	"$(BOOL_SEARCH_BIN)" "$$DIR" '$(Q)' --limit "$(LIMIT)" --stemming "$$S"

full: deps download tokenize zipf index
	@echo "OK: full pipeline done"

build_cpp: $(TOKEN_STATS_BIN) $(TERM_FREQ_BIN) $(BOOL_INDEX_BIN) $(BOOL_SEARCH_BIN)

$(BIN_DIR):
	mkdir -p "$(BIN_DIR)"

$(TOKEN_STATS_BIN): $(BIN_DIR) $(CPP_DIR)/text_token_stats.cpp $(CPP_DIR)/text_tokenizer.cpp $(CPP_DIR)/word_stemmer.cpp $(CPP_DIR)/fs_utils.cpp
	g++ -O2 -std=c++17 -o "$@" $^

$(TERM_FREQ_BIN): $(BIN_DIR) $(CPP_DIR)/term_frequency.cpp $(CPP_DIR)/text_tokenizer.cpp $(CPP_DIR)/word_stemmer.cpp $(CPP_DIR)/fs_utils.cpp
	g++ -O2 -std=c++17 -o "$@" $^

$(BOOL_INDEX_BIN): $(BIN_DIR) $(CPP_DIR)/boolean_index_builder.cpp $(CPP_DIR)/text_tokenizer.cpp $(CPP_DIR)/word_stemmer.cpp $(CPP_DIR)/fs_utils.cpp
	g++ -O2 -std=c++17 -o "$@" $^

$(BOOL_SEARCH_BIN): $(BIN_DIR) $(CPP_DIR)/boolean_search_cli.cpp $(CPP_DIR)/text_tokenizer.cpp $(CPP_DIR)/word_stemmer.cpp $(CPP_DIR)/fs_utils.cpp
	g++ -O2 -std=c++17 -o "$@" $^

clean:
	rm -rf "$(BIN_DIR)" .venv

clean_index:
	@rm -f "$(TOKENIZE_MARK)" "$(ACTIVE_STEM_FILE)"
	@rm -f "$(DOCS_LIST)" "$(DOCS_LIST_ABS)" "$(META_DOCID)"
	@rm -f "$(OUT_DIR)"/token_stats_s*.txt
	@rm -f "$(OUT_DIR)"/termfreq_s*.tsv "$(OUT_DIR)"/termfreq_s*.log
	@rm -f "$(OUT_DIR)"/zipf_s*.csv "$(OUT_DIR)"/zipf_s*.png
	@rm -rf "$(OUT_DIR)"/boolean_index_s*
	@echo "OK: cleaned"
