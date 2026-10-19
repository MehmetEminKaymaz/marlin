#!/usr/bin/env python3
"""Paketteki üçüncü taraf yazılımların lisans bildirimini üretir.

Kurulumun son adımında çalışır (cmake/BundleDependencies.cmake). Pakete giren
her parçanın lisansı, metniyle birlikte THIRD_PARTY_NOTICES.txt dosyasına
yazılır; Apache-2.0, BSD ve MIT lisansları ikili dağıtımda bunu ister.

Parçalar iki yoldan gelir:
  * paylaşımlı kütüphaneler (--bundled): pakete kopyalanan dosyalar. Hangi
    conda paketinden geldikleri conda ortamının conda-meta/*.json
    kayıtlarından bulunur; lisans metni paketin info/licenses dizinindedir.
  * derleyiciye ya da çalışma zamanına statik giren parçalar (--conda-static:
    conda paket adları, --extra: "ad|lisans|metin dosyası" üçlüleri).

Conda ortamı yoksa (yerel geliştirme derlemesi) yalnızca --extra yazılır.
Kullanım:
  third_party_notices.py --out <dosya> [--conda-prefix P] [--bundled f ...]
                         [--conda-static ad ...] [--extra "ad|lisans|dosya" ...]
"""

import argparse
import glob
import json
import os
import sys

HEADER = """Marlin, MIT lisansıyla dağıtılır (bkz. LICENSE). Bu paket aşağıdaki
üçüncü taraf yazılımları içerir; her birinin lisansı ve lisans metni
aşağıdadır.

Marlin is distributed under the MIT license (see LICENSE). This package
contains the third-party software listed below, each under its own license.
"""


# Lisans dosyası taşımayan bilinen conda paketleri: conda-forge'da GCC'nin
# çalışma zamanı kütüphaneleri ayrı paketlerdir ve info/licenses içermez.
GCC_RUNTIME = """Part of GCC, the GNU Compiler Collection. Licensed under the GNU General
Public License version 3 with the GCC Runtime Library Exception, version 3.1:
  https://www.gnu.org/licenses/gpl-3.0.html
  https://www.gnu.org/licenses/gcc-exception-3.1.html
Source code: https://gcc.gnu.org/ (conda-forge build:
https://github.com/conda-forge/ctng-compilers-feedstock)."""
KNOWN_LICENSES = {name: GCC_RUNTIME for name in
                  ("libgcc", "libgcc-ng", "libstdcxx", "libstdcxx-ng", "libgomp")}


def read_text(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().strip()


def conda_records(prefix):
    """conda-meta kayıtları: (kayıt, dosya yollarının kümesi)."""
    records = []
    for meta in glob.glob(os.path.join(prefix, "conda-meta", "*.json")):
        try:
            with open(meta, encoding="utf-8") as f:
                record = json.load(f)
        except (OSError, ValueError):
            continue
        files = {p.replace("\\", "/").lower() for p in record.get("files", [])}
        records.append((record, files))
    return records


def package_dirs(record, prefix):
    """Paketin açıldığı dizin adayları (lisans metinleri info/licenses'ta)."""
    candidates = []
    if record.get("extracted_package_dir"):
        candidates.append(record["extracted_package_dir"])
    stem = record.get("fn", "")
    for ext in (".conda", ".tar.bz2"):
        if stem.endswith(ext):
            stem = stem[: -len(ext)]
    if not stem:
        stem = "%s-%s-%s" % (record.get("name"), record.get("version"), record.get("build"))
    roots = [os.environ.get("MAMBA_ROOT_PREFIX", ""), os.environ.get("CONDA_PKGS_DIRS", "")]
    # (Ortam <kök>/envs/<ad> altındaysa önbellek <kök>/pkgs'tir.)
    roots.append(os.path.dirname(os.path.dirname(os.path.abspath(prefix))))
    for root in filter(None, roots):
        for pkgs in (root, os.path.join(root, "pkgs")):
            candidates.append(os.path.join(pkgs, stem))
    return candidates


def license_texts(record, prefix, report_missing=False):
    if report_missing:
        print("third_party_notices: %s icin bakilan yerler: %s" % (record.get("name"), ", ".join(package_dirs(record, prefix))),
              file=sys.stderr)
    for directory in package_dirs(record, prefix):
        licenses = os.path.join(directory, "info", "licenses")
        if os.path.isdir(licenses):
            texts = []
            for path in sorted(glob.glob(os.path.join(licenses, "**", "*"), recursive=True)):
                if os.path.isfile(path):
                    texts.append("--- %s ---\n%s" % (os.path.relpath(path, licenses), read_text(path)))
            if texts:
                return texts
    return []


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--conda-prefix", default=os.environ.get("CONDA_PREFIX", ""))
    parser.add_argument("--bundled", nargs="*", default=[])
    parser.add_argument("--conda-static", nargs="*", default=[])
    parser.add_argument("--extra", nargs="*", default=[])
    parser.add_argument("--strict", action="store_true",
                        help="lisans metni bulunamayan parça hatadır (paket derlemesi)")
    args = parser.parse_args()

    entries = []  # (ad, sürüm, lisans, içerdiği dosyalar, metinler)
    missing = []

    if args.conda_prefix and os.path.isdir(os.path.join(args.conda_prefix, "conda-meta")):
        records = conda_records(args.conda_prefix)
        by_name = {r["name"]: (r, files) for r, files in records}
        owners = {}  # paket adı -> içerdiği paket dosyaları
        for path in args.bundled:
            real = os.path.realpath(path)
            found = False
            for candidate in (path, real):
                rel = os.path.relpath(candidate, args.conda_prefix).replace("\\", "/").lower()
                if rel.startswith(".."):
                    continue
                for record, files in records:
                    if rel in files:
                        owners.setdefault(record["name"], set()).add(os.path.basename(path))
                        found = True
                        break
                if found:
                    break
            if not found:
                missing.append("%s: hangi conda paketinden geldigi bulunamadi" % path)
        for name in args.conda_static:
            if name in by_name:
                owners.setdefault(name, set()).add("(derleyiciye statik olarak bagli)")
        for name in sorted(owners):
            record, _ = by_name[name]
            texts = license_texts(record, args.conda_prefix)
            if not texts and name in KNOWN_LICENSES:
                texts = [KNOWN_LICENSES[name]]
            if not texts:
                # (Yalnızca ilk eksik için: nerelere bakıldığı teşhis içindir.)
                if not any("lisans metni" in m for m in missing):
                    license_texts(record, args.conda_prefix, report_missing=True)
                missing.append("%s: lisans metni bulunamadi" % name)
            entries.append((name, record.get("version", ""), record.get("license", "?"),
                            sorted(owners[name]), texts))
    elif args.bundled:
        missing.append("conda ortami yok: paketteki kutuphanelerin lisanslari belirlenemedi")

    for extra in args.extra:
        name, license_id, path = extra.split("|", 2)
        texts = []
        if path and os.path.isfile(path):
            texts.append(read_text(path))
        elif path and not os.path.isfile(path) and not path.startswith("@"):
            missing.append("%s: lisans dosyasi yok (%s)" % (name, path))
        if path.startswith("@"):
            texts.append(path[1:])  # metnin kendisi
        entries.append((name, "", license_id, [], texts))

    with open(args.out, "w", encoding="utf-8", newline="\n") as out:
        out.write(HEADER + "\n")
        out.write("=" * 78 + "\n")
        for name, version, license_id, files, _ in entries:
            line = "  %-28s %-14s %s" % (name, version, license_id)
            out.write(line.rstrip() + "\n")
        for name, version, license_id, files, texts in entries:
            out.write("\n" + "=" * 78 + "\n")
            out.write("%s %s\nLicense: %s\n" % (name, version, license_id))
            if files:
                out.write("Files: %s\n" % ", ".join(files))
            out.write("\n" + ("\n\n".join(texts) if texts else "(license text not available)") + "\n")

    for problem in missing:
        print("third_party_notices: " + problem, file=sys.stderr)
    print("third_party_notices: %d parca -> %s" % (len(entries), args.out))
    return 1 if (args.strict and missing) else 0


if __name__ == "__main__":
    sys.exit(main())
