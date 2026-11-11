import ftplib
import os
import sys

FTP_HOST = "192.168.0.7"
FTP_PORT = 5000
LOCAL_DIST = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "dist"))

def ensure_dir(ftp, remote_dir):
    parts = remote_dir.strip("/").split("/")
    current = ""
    for p in parts:
        current += "/" + p
        try:
            ftp.mkd(current)
            print(f"Created dir: {current}")
        except Exception:
            pass

def upload_file(ftp, local_path, remote_path):
    print(f"Uploading {local_path} -> {remote_path}...")
    with open(local_path, "rb") as f:
        ftp.storbinary(f"STOR {remote_path}", f)
    size = os.path.getsize(local_path)
    print(f"Uploaded {size} bytes.")

def main():
    print(f"Connecting to FTP {FTP_HOST}:{FTP_PORT}...")
    ftp = ftplib.FTP()
    ftp.connect(FTP_HOST, FTP_PORT, timeout=10)
    ftp.login()
    print("Logged in successfully.")

    files_to_upload = [
        ("atmosphere/contents/4200000000004150/exefs.nsp", "atmosphere/contents/4200000000004150/exefs.nsp"),
        ("atmosphere/contents/4200000000004150/toolbox.json", "atmosphere/contents/4200000000004150/toolbox.json"),
        ("atmosphere/contents/4200000000004150/flags/boot2.flag", "atmosphere/contents/4200000000004150/flags/boot2.flag"),
        ("config/sys-autopilot/config.ini", "config/sys-autopilot/config.ini"),
        ("switch/sys-autopilot/sys-autopilot.nro", "switch/sys-autopilot/sys-autopilot.nro"),
    ]

    for rel_local, rel_remote in files_to_upload:
        local_full = os.path.join(LOCAL_DIST, rel_local)
        remote_dir = os.path.dirname(rel_remote)
        ensure_dir(ftp, remote_dir)
        upload_file(ftp, local_full, rel_remote)

    print("\nVerifying uploaded files on Switch:")
    for _, rel_remote in files_to_upload:
        try:
            size = ftp.size(rel_remote)
            print(f"  {rel_remote} - {size} bytes [OK]")
        except Exception as e:
            print(f"  {rel_remote} - error: {e}")

    ftp.quit()
    print("All files transferred successfully!")

if __name__ == "__main__":
    main()
