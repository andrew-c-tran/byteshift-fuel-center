#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ByteShift Labs LLC
"""Check every part in kicad/jlc_bom.csv against live JLCPCB assembly stock.

Usage: python3 hardware/check_stock.py [boards]   (default 50)
Exits non-zero if any part cannot cover the requested number of boards.
Uses the JSON endpoint behind jlcpcb.com/parts; it is not a documented API and may change."""
import csv, json, os, sys, urllib.request

URL = 'https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList'
BOM = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'kicad', 'jlc_bom.csv')


def jlc_part(code):
    body = json.dumps({'keyword': code, 'currentPage': 1, 'pageSize': 10}).encode()
    req = urllib.request.Request(URL, data=body, headers={'Content-Type': 'application/json'})
    found = json.load(urllib.request.urlopen(req, timeout=30))['data']['componentPageInfo']['list'] or []
    return next((p for p in found if p['componentCode'] == code), None)


def main():
    boards = int(sys.argv[1]) if len(sys.argv) > 1 else 50
    short = 0
    for row in csv.DictReader(open(BOM, newline='')):
        code, refs = row['LCSC Part #'], row['Designator']
        need = boards * len(refs.split(','))
        part = jlc_part(code)
        stock = part['stockCount'] if part else 0
        ok = stock >= need
        short += not ok
        model = part['componentModelEn'] if part else 'not found'
        print(f"{'ok   ' if ok else 'SHORT'} {code:9} {model:26} stock {stock:>8}  need {need:>5}  {refs}")
    print(f"\n{short} part(s) short for {boards} boards" if short else f"\nall parts in stock for {boards} boards")
    sys.exit(1 if short else 0)


if __name__ == '__main__':
    main()
