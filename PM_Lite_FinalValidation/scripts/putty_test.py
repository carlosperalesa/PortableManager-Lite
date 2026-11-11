"""Guided D3 transaction; RAM snapshot must survive both GUI sessions."""
from registry_transaction import Transaction
KEY = r"Software\SimonTatham\PuTTY"

def begin():
    tx=Transaction([KEY])
    if not tx.begin(KEY): raise RuntimeError("Existing PuTTY key: stop and request user authorization")
    return tx

if __name__ == "__main__":
    raise SystemExit("Pending USB confirmation; no application launched or registry changed")
