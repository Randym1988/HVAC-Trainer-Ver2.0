import argparse
import contextlib
import os
from getpass import getpass

import main

MIN_PASSWORD_LENGTH = 14


def _validate_username(username: str) -> str:
    normalized = username.strip()
    if not 3 <= len(normalized) <= 64:
        raise ValueError("Username must be 3-64 characters")
    return normalized


def _validate_password(password: str) -> None:
    if len(password) < MIN_PASSWORD_LENGTH:
        raise ValueError(f"Password must be at least {MIN_PASSWORD_LENGTH} characters")


def bootstrap_admin(username: str, password: str) -> None:
    username = _validate_username(username)
    _validate_password(password)
    users = main.load_users_db()
    if users:
        raise ValueError("Users already exist; use set-password to rotate an account")

    users[username] = {
        "pw_hash": main.hash_password(password),
        "role": "admin",
    }
    main.write_json_atomically(main.USERS_DB_FILE, users)


def set_password(username: str, password: str) -> None:
    username = _validate_username(username)
    _validate_password(password)
    users = main.load_users_db()
    record = users.get(username)
    if not isinstance(record, dict):
        raise ValueError(f"User {username!r} does not exist")

    record.pop("pw", None)
    record["pw_hash"] = main.hash_password(password)
    users[username] = record
    main.write_json_atomically(main.USERS_DB_FILE, users)


def delete_user(username: str) -> None:
    username = _validate_username(username)
    users = main.load_users_db()
    main.ensure_user_removable(users, username)

    del users[username]
    main.write_json_atomically(main.USERS_DB_FILE, users)


def list_users(show_hashes: bool = False) -> list[str]:
    users = main.load_users_db()
    lines = [f"{'USER':<24}{'ROLE':<12}PASSWORD HASH"]
    for username in sorted(users, key=str.lower):
        record = users[username]
        if not isinstance(record, dict):
            continue
        if show_hashes:
            stored = str(record.get("pw_hash") or "(plaintext - rotate required)")
        else:
            stored = main.describe_user_record(username, record)["hash_scheme"]
        role = str(record.get("role", "student"))
        lines.append(f"{username:<24}{role:<12}{stored}")
    return lines


def export_users(path: str) -> int:
    """Write every account and its salted password hash to a private JSON backup."""
    users = main.load_users_db()
    exported = {
        name: {"role": record.get("role", "student"), "pw_hash": record.get("pw_hash")}
        for name, record in users.items()
        if isinstance(record, dict)
    }
    main.write_json_atomically(path, exported)
    with contextlib.suppress(OSError):
        os.chmod(path, 0o600)
    return len(exported)


def _prompt_password() -> str:
    password = getpass("New password (input hidden): ")
    confirmation = getpass("Confirm new password: ")
    if password != confirmation:
        raise ValueError("Passwords do not match")
    _validate_password(password)
    return password


def main_cli() -> int:
    parser = argparse.ArgumentParser(description="Manage Vexera Core local users")
    commands = parser.add_subparsers(dest="command", required=True)

    bootstrap_command = commands.add_parser("bootstrap-admin")
    bootstrap_command.add_argument("username")

    rotate_command = commands.add_parser("set-password")
    rotate_command.add_argument("username")

    delete_command = commands.add_parser("delete-user")
    delete_command.add_argument("username")

    list_command = commands.add_parser("list-users")
    list_command.add_argument(
        "--show-hashes", action="store_true", help="print full salted password hashes"
    )

    export_command = commands.add_parser("export-users")
    export_command.add_argument("path", help="backup file to write (contains hashes)")

    arguments = parser.parse_args()
    try:
        if arguments.command == "list-users":
            print("\n".join(list_users(arguments.show_hashes)))
            return 0
        if arguments.command == "export-users":
            count = export_users(arguments.path)
            print(f"Exported {count} accounts to {arguments.path}")
            return 0
        if arguments.command == "bootstrap-admin":
            bootstrap_admin(arguments.username, _prompt_password())
        elif arguments.command == "set-password":
            set_password(arguments.username, _prompt_password())
        else:
            delete_user(arguments.username)
    except ValueError as error:
        parser.error(str(error))

    print(f"User operation '{arguments.command}' completed for {arguments.username}.")
    print("Restart the engine container to revoke in-memory sessions and reload users.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main_cli())
