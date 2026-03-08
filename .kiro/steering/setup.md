---
inclusion: manual
---

# Project Setup

## Inputs

Ask the user before starting. If the project already has a `CMakeLists.txt` with no `{...}` placeholders, skip this entire setup — do not ask for inputs and do not run any of the steps below.

| Input | Example |
|-------|---------|
| Project name | `acme-utils` |
| Project type | `library` or `application` |
| Description | One-line project summary |
| Author | Name for LICENSE |
| Email | Email for LICENSE |
| Year | Year or range for LICENSE (e.g. `2026-2036`) |

## Steps

### 1. Copy Templates

If the project root is empty or not yet initialized:

1. Locate the `common/` and type-specific (`library/` or `application/`) template directories. They may be in this skill's directory or already copied into the project.
2. Copy `common/` into the project root.
3. Copy the matching type directory on top.

If template files are already present in the project, skip to step 2.

### 2. Replace Placeholders

| Placeholder | Value | Derivation |
|-------------|-------|------------|
| `{project}` | project name, lowercase with hyphens | As-is from user input (e.g. `acme-utils`) |
| `{PROJECT}` | uppercase, hyphens → underscores | e.g. `ACME_UTILS` |
| `{YEAR}` | current year | e.g. `2026` |
| `{AUTHOR}` | author name | From user input |
| `{EMAIL}` | email address | From user input |
| `{DESCRIPTION}` | one-line description | From user input |

Replace in all files. No `{...}` placeholder should remain after this step.

### 3. Rename Files

| From | To |
|------|----|
| `cmake/utils.cmake` | `cmake/{project}-utils.cmake` |
| `include/project.h` (library only) | `include/{project}.h` |

### 4. Verify

Search all files for the case-insensitive regex `\{(project|year|author|email|description)\}` to confirm no unreplaced placeholders remain.
