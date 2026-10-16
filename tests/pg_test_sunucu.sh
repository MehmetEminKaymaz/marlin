#!/bin/bash
# std.postgres testleri için yerel bir PostgreSQL sunucusu (yönetici izni
# gerekmez; veriler ~/marlin-deps/pgdata altında durur).
#
#   bash tests/pg_test_sunucu.sh          kurar (ilk seferde) ve başlatır
#   bash tests/pg_test_sunucu.sh dur      durdurur
#
# Sunucu programı önce sistemde (sudo apt install postgresql), yoksa
# ~/marlin-deps altında aranır. Oraya yönetici izni olmadan açmak için:
#
#   mkdir -p ~/marlin-deps/debs && cd ~/marlin-deps/debs
#   apt-get download postgresql-16 postgresql-client-16 libpq5 libpq-dev libsqlite3-dev
#   for d in *.deb; do dpkg -x "$d" ~/marlin-deps/root; done
#
# Başladıktan sonra testler için:
#
#   export MARLIN_PG_TEST="host=localhost port=54329 dbname=marlin_test user=marlin password=marlin"
#   bash tests/run.sh

DEPS="$HOME/marlin-deps"
BIN=/usr/lib/postgresql/16/bin
SHARE=/usr/share/postgresql/16
if [ ! -x "$BIN/postgres" ]; then
    BIN="$DEPS/root/usr/lib/postgresql/16/bin"
    SHARE="$DEPS/root/usr/share/postgresql/16"
    export LD_LIBRARY_PATH="$DEPS/root/usr/lib/x86_64-linux-gnu"
fi
if [ ! -x "$BIN/postgres" ]; then
    echo "PostgreSQL sunucusu bulunamadi (bu dosyanin basindaki aciklamaya bakin)"
    exit 1
fi
DATA="$DEPS/pgdata"
PORT=54329

if [ "$1" = dur ]; then
    "$BIN/pg_ctl" -D "$DATA" stop -m fast
    exit
fi

if [ ! -d "$DATA" ]; then
    mkdir -p "$DEPS"
    PW=$(mktemp)
    echo marlin > "$PW"
    "$BIN/initdb" -D "$DATA" -U marlin --pwfile="$PW" -A scram-sha-256 -L "$SHARE" > "$DEPS/initdb.log" 2>&1
    code=$?
    rm -f "$PW"
    [ $code = 0 ] || { tail "$DEPS/initdb.log"; exit 1; }
fi

mkdir -p /tmp/marlin-pg
"$BIN/pg_ctl" -D "$DATA" -l "$DEPS/pg.log" -o "-p $PORT -k /tmp/marlin-pg -c listen_addresses=localhost" start || exit 1
sleep 1
export PGPASSWORD=marlin
"$BIN/psql" -h localhost -p $PORT -U marlin -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname = 'marlin_test'" | grep -q 1 ||
    "$BIN/psql" -h localhost -p $PORT -U marlin -d postgres -qc "CREATE DATABASE marlin_test"
echo "hazir: export MARLIN_PG_TEST=\"host=localhost port=$PORT dbname=marlin_test user=marlin password=marlin\""
