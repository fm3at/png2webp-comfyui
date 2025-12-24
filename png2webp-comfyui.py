import os
import json
from PIL import Image
from PIL.PngImagePlugin import PngInfo
import sys
from tqdm import tqdm
from datetime import datetime

def extract_png_metadata(png_path):
    """
    Извлекает 'prompt' и 'workflow' из tEXt-чанков PNG.
    Возвращает словарь с ключами: 'prompt', 'workflow', и 'extra_pnginfo' (если есть).
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
        print(f"⚠️ Ошибка при чтении метаданных из {png_path}: {e}")
        return {}


def create_exif_for_webp(metadata_dict):
    """
    Создаёт EXIF-объект PIL, заполняя его тегами в формате ComfyUI:
      - prompt: -> 0x0110 (UserComment)
      - workflow: -> 0x010f (ImageDescription)
      - extra_pnginfo keys -> 0x010e, 0x010d, ... (в обратном порядке)
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


def save_webp_with_metadata(png_path, output_path, quality=80, method=4, lossless=False):
    """
    Конвертирует PNG в WEBP с сохранением метаданных в EXIF.
    Возвращает True при успехе, False при ошибке.
    """
    try:
        img = Image.open(png_path)
        metadata_dict = extract_png_metadata(png_path)
        exif = create_exif_for_webp(metadata_dict)

        # Сохраняем WEBP
        img.save(
            output_path,
            format='WEBP',
            quality=quality,
            method=method,
            lossless=lossless,
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
            print(f"   📦 Перенесено: {saved_keys}")
        else:
            print("   📦 Нет метаданных для переноса")

        return True

    except Exception as e:
        print(f"❌ Ошибка при конвертации {png_path}: {e}")
        return False


def process_directory(directory):
    """
    Рекурсивно находит все PNG-файлы и возвращает список с путями.
    """
    png_files = []
    for root, _, files in os.walk(directory):
        for file in files:
            if file.lower().endswith('.png'):
                png_files.append(os.path.join(root, file))
    return png_files


def get_creation_date(png_path):
    """
    Возвращает дату создания файла в формате YYYY_MM_DD.
    Использует время создания (ctime) как fallback, если modification time недоступен.
    """
    try:
        # Получаем время создания файла (на Windows — ctime, на Unix — иногда тоже ctime)
        # В большинстве случаев это то, что нужно для сортировки по дате создания
        timestamp = os.path.getctime(png_path)
        return datetime.fromtimestamp(timestamp).strftime('%Y_%m_%d')
    except Exception:
        # Если не получилось — возвращаем текущую дату как fallback
        return datetime.now().strftime('%Y_%m_%d')


def main():
    if len(sys.argv) < 2:
        print("🔹 Использование: Перетащите PNG-файл или папку на эту иконку.")
        print("🔹 Скрипт конвертирует все .png в .webp с сохранением метаданных ComfyUI")
        print("🔹 Все файлы сохраняются в подпапки `webp/YYYY_MM_DD/` в корне исходной папки")
        input("\nНажмите Enter для выхода...")
        return

    path = sys.argv[1]

    if not os.path.exists(path):
        print(f"❌ Указанный путь не существует: {path}")
        input("\nНажмите Enter для выхода...")
        return

    # Определяем, файл это или папка
    if os.path.isfile(path) and path.lower().endswith('.png'):
        files_to_convert = [path]
        base_dir = os.path.dirname(path)
        print(f"📄 Обработка одного файла: {path}")
    elif os.path.isdir(path):
        print(f"📁 Обработка папки: {path}")
        files_to_convert = process_directory(path)
        base_dir = path
        print(f"   Найдено {len(files_to_convert)} PNG-файлов.")
    else:
        print(f"❌ Указанный путь не является PNG-файлом или папкой: {path}")
        input("\nНажмите Enter для выхода...")
        return

    if not files_to_convert:
        print("ℹ️ Нет PNG-файлов для конвертации.")
        input("\nНажмите Enter для выхода...")
        return

    # Определяем корневую папку для сохранения: base_dir/webp/
    webp_root = os.path.join(base_dir, "webp")
    os.makedirs(webp_root, exist_ok=True)

    converted_count = 0
    failed_count = 0

    # Обработка с прогресс-баром
    for png_path in tqdm(files_to_convert, desc="🔄 Конвертация PNG → WEBP", unit="файл"):
        try:
            # Получаем дату создания файла
            date_folder = get_creation_date(png_path)
            subfolder_path = os.path.join(webp_root, date_folder)
            os.makedirs(subfolder_path, exist_ok=True)

            # Генерируем имя файла: сохраняем исходное имя, но с .webp
            filename = os.path.basename(png_path)
            output_path = os.path.join(subfolder_path, os.path.splitext(filename)[0] + ".webp")

            # Конвертируем
            success = save_webp_with_metadata(png_path, output_path)

            if success:
                converted_count += 1
            else:
                failed_count += 1

        except Exception as e:
            print(f"❌ Ошибка при обработке {png_path}: {e}")
            failed_count += 1

    print(f"\n✅ Готово! Успешно: {converted_count}, Ошибки: {failed_count}")
    print(f"📁 Все файлы сохранены в: {webp_root}")

    # Пауза перед закрытием
    input("\nНажмите Enter, чтобы закрыть окно...")


if __name__ == "__main__":
    main()
