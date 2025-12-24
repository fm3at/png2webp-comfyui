import os
import json
from PIL import Image
from PIL.PngImagePlugin import PngInfo
import sys
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed
import multiprocessing

# Try to import tqdm, fall back to simple progress if not available
try:
    from tqdm import tqdm
    HAS_TQDM = True
except ImportError:
    HAS_TQDM = False


def extract_png_metadata(png_path):
    """
    Extracts 'prompt' and 'workflow' from tEXt chunks in PNG.
    Returns dict with keys: 'prompt', 'workflow', 'extra_pnginfo' (if present).
    """
    try:
        img = Image.open(png_path)
        if not hasattr(img, 'text'):
            return {}

        metadata = {}
        for key in ['prompt', 'workflow']:
            if key in img.text:
                try:
                    metadata[key] = json.loads(img.text[key])
                except json.JSONDecodeError:
                    metadata[key] = img.text[key]

        if 'extra_pnginfo' in img.text:
            try:
                metadata['extra_pnginfo'] = json.loads(img.text['extra_pnginfo'])
            except json.JSONDecodeError:
                metadata['extra_pnginfo'] = img.text['extra_pnginfo']

        return metadata

    except Exception as e:
        print(f"⚠️ Error reading metadata from {png_path}: {e}")
        return {}


def create_exif_for_webp(metadata_dict):
    """
    Creates PIL Exif object with ComfyUI-compatible tags:
      - prompt -> 0x0110 (UserComment)
      - workflow -> 0x010f (ImageDescription)
      - extra_pnginfo keys -> 0x010e, 0x010d, ... (in reverse order)
    """
    from PIL import Image

    exif = Image.Exif()

    if 'prompt' in metadata_dict:
        value = json.dumps(metadata_dict['prompt'], ensure_ascii=False) if isinstance(metadata_dict['prompt'], dict) else str(metadata_dict['prompt'])
        exif[0x0110] = f"prompt:{value}"

    if 'workflow' in metadata_dict:
        value = json.dumps(metadata_dict['workflow'], ensure_ascii=False) if isinstance(metadata_dict['workflow'], dict) else str(metadata_dict['workflow'])
        exif[0x010f] = f"workflow:{value}"

    if 'extra_pnginfo' in metadata_dict and isinstance(metadata_dict['extra_pnginfo'], dict):
        tag_id = 0x010e
        for key, value in metadata_dict['extra_pnginfo'].items():
            json_value = json.dumps(value, ensure_ascii=False) if isinstance(value, (dict, list)) else str(value)
            exif[tag_id] = f"{key}:{json_value}"
            tag_id -= 1

    return exif


def save_webp_with_metadata(args):
    """
    Worker function for ProcessPoolExecutor.
    Expects tuple: (png_path, output_path)
    Returns (success: bool, png_path: str)
    """
    png_path, output_path = args
    try:
        img = Image.open(png_path)
        metadata_dict = extract_png_metadata(png_path)
        exif = create_exif_for_webp(metadata_dict)

        img.save(
            output_path,
            format='WEBP',
            quality=80,
            method=4,
            lossless=False,
            exif=exif,
            optimize=True
        )

        saved_keys = []
        if 'prompt' in metadata_dict:
            saved_keys.append('prompt')
        if 'workflow' in metadata_dict:
            saved_keys.append('workflow')
        if 'extra_pnginfo' in metadata_dict and isinstance(metadata_dict['extra_pnginfo'], dict):
            saved_keys.extend([f"extra_{k}" for k in metadata_dict['extra_pnginfo']])

        if saved_keys:
            print(f"   📦 Metadata saved: {saved_keys}")
        else:
            print(f"   📦 No metadata to save from {os.path.basename(png_path)}")

        return True, png_path

    except Exception as e:
        print(f"❌ Error converting {png_path}: {e}")
        return False, png_path


def process_directory(directory):
    """
    Recursively finds all .png files in directory and subdirectories.
    Returns list of full file paths.
    """
    png_files = []
    for root, _, files in os.walk(directory):
        for file in files:
            if file.lower().endswith('.png'):
                png_files.append(os.path.join(root, file))
    return png_files


def get_creation_date(png_path):
    """
    Returns file creation date as 'YYYY_MM_DD' string.
    Uses ctime (creation time) as fallback if mtime is not reliable.
    """
    try:
        timestamp = os.path.getctime(png_path)
        return datetime.fromtimestamp(timestamp).strftime('%Y_%m_%d')
    except Exception:
        # Fallback to current date if timestamp is unavailable
        return datetime.now().strftime('%Y_%m_%d')


def main():
    if len(sys.argv) < 2:
        print("🔹 Usage: Drag and drop a .png file or folder onto this script.")
        print("🔹 All converted .webp files will be saved in 'webp/YYYY_MM_DD/' folders.")
        print("🔹 Metadata (prompt, workflow, extra_pnginfo) is preserved in EXIF.")
        input("\nPress Enter to exit...")
        return

    path = sys.argv[1]

    if not os.path.exists(path):
        print(f"❌ Path does not exist: {path}")
        input("\nPress Enter to exit...")
        return

    # Determine if it's a file or directory
    if os.path.isfile(path) and path.lower().endswith('.png'):
        files_to_convert = [path]
        base_dir = os.path.dirname(path)
        print(f"📄 Processing single file: {path}")
    elif os.path.isdir(path):
        print(f"📁 Processing folder: {path}")
        files_to_convert = process_directory(path)
        base_dir = path
        print(f"   Found {len(files_to_convert)} PNG files.")
    else:
        print(f"❌ Path is not a .png file or directory: {path}")
        input("\nPress Enter to exit...")
        return

    if not files_to_convert:
        print("ℹ️ No PNG files found to convert.")
        input("\nPress Enter to exit...")
        return

    # Define output root folder
    webp_root = os.path.join(base_dir, "webp")
    os.makedirs(webp_root, exist_ok=True)

    # Prepare argument list for multiprocessing
    tasks = []
    for png_path in files_to_convert:
        date_folder = get_creation_date(png_path)
        subfolder_path = os.path.join(webp_root, date_folder)
        os.makedirs(subfolder_path, exist_ok=True)

        filename = os.path.basename(png_path)
        output_path = os.path.join(subfolder_path, os.path.splitext(filename)[0] + ".webp")
        tasks.append((png_path, output_path))

    # Determine number of worker processes (all CPU cores)
    num_workers = multiprocessing.cpu_count()
    print(f"⚙️ Using {num_workers} CPU cores for parallel conversion...")

    converted_count = 0
    failed_count = 0

    # Handle progress display with or without tqdm
    if HAS_TQDM:
        iterable = tqdm(tasks, desc="🔄 Converting PNG → WEBP", unit="file")
    else:
        print("ℹ️ tqdm not installed. Using basic progress output.")
        iterable = tasks

    # Use ProcessPoolExecutor for parallel processing
    with ProcessPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(save_webp_with_metadata, task): task for task in tasks}

        for future in as_completed(futures):
            success, png_path = future.result()
            if success:
                converted_count += 1
            else:
                failed_count += 1

            # tqdm updates inside the loop
            if HAS_TQDM:
                iterable.update(1)

    print(f"\n✅ Done! Converted: {converted_count}, Failed: {failed_count}")
    print(f"📁 Output folder: {webp_root}")

    input("\nPress Enter to exit...")


if __name__ == "__main__":
    main()
