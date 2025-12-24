import os
import json
from PIL import Image
from PIL.PngImagePlugin import PngInfo
import sys
from tqdm import tqdm

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
                    # Пытаемся десериализовать как JSON
                    metadata[key] = json.loads(img.text[key])
                except json.JSONDecodeError:
                    # Если не JSON — сохраняем как строку
                    metadata[key] = img.text[key]

        # Для совместимости с ComfyUI: если есть 'extra_pnginfo' в tEXt — тоже извлекаем
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

    Возвращает объект Image.Exif
    """
    from PIL import Image

    exif = Image.Exif()

    # Записываем prompt в 0x0110 (UserComment)
    if 'prompt' in metadata_dict:
        value = json.dumps(metadata_dict['prompt'], ensure_ascii=False) if isinstance(metadata_dict['prompt'], dict) else str(metadata_dict['prompt'])
        exif[0x0110] = f"prompt:{value}"

    # Записываем workflow в 0x010f (ImageDescription)
    if 'workflow' in metadata_dict:
        value = json.dumps(metadata_dict['workflow'], ensure_ascii=False) if isinstance(metadata_dict['workflow'], dict) else str(metadata_dict['workflow'])
        exif[0x010f] = f"workflow:{value}"

    # Записываем extra_pnginfo в теги 0x010e, 0x010d, ... (в обратном порядке)
    if 'extra_pnginfo' in metadata_dict and isinstance(metadata_dict['extra_pnginfo'], dict):
        tag_id = 0x010e  # Начинаем с ImageDescription-1
        for key, value in metadata_dict['extra_pnginfo'].items():
            json_value = json.dumps(value, ensure_ascii=False) if isinstance(value, (dict, list)) else str(value)
            exif[tag_id] = f"{key}:{json_value}"
            tag_id -= 1  # Уменьшаем ID для следующего тега

    return exif


def save_webp_with_metadata(png_path, output_path=None, quality=80, method=4, lossless=False):
    """
    Конвертирует PNG в WEBP, перенося метаданные в EXIF-теги в формате ComfyUI.
    Использует теги:
      - prompt: -> 0x0110 (UserComment)
      - workflow: -> 0x010f (ImageDescription)
      - extra_pnginfo keys -> 0x010e, 0x010d, ... (в обратном порядке)
    """
    try:
        # Открываем PNG
        img = Image.open(png_path)

        # Извлекаем метаданные
        metadata_dict = extract_png_metadata(png_path)

        # Создаём EXIF с корректными тегами
        exif = create_exif_for_webp(metadata_dict)

        # Если output_path не задан — генерируем на основе png_path
        if output_path is None:
            output_path = os.path.splitext(png_path)[0] + ".webp"

        # Сохраняем как WEBP с EXIF
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
    Рекурсивно обходит директорию и конвертирует все PNG в WEBP.
    Возвращает список всех найденных PNG-файлов.
    """
    png_files = []
    for root, _, files in os.walk(directory):
        for file in files:
            if file.lower().endswith('.png'):
                png_files.append(os.path.join(root, file))
    return png_files


def main():
    # Проверяем, был ли файл/папка передан через drag & drop
    if len(sys.argv) < 2:
        print("🔹 Использование: Перетащите PNG-файл или папку на эту иконку.")
        print("🔹 Скрипт конвертирует все .png в .webp с сохранением метаданных ComfyUI.")
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
        print(f"📄 Обработка одного файла: {path}")
    elif os.path.isdir(path):
        print(f"📁 Обработка папки: {path}")
        files_to_convert = process_directory(path)
        print(f"   Найдено {len(files_to_convert)} PNG-файлов.")
    else:
        print(f"❌ Указанный путь не является PNG-файлом или папкой: {path}")
        input("\nНажмите Enter для выхода...")
        return

    if not files_to_convert:
        print("ℹ️ Нет PNG-файлов для конвертации.")
        input("\nНажмите Enter для выхода...")
        return

    # Обработка с прогресс-баром tqdm
    converted_count = 0
    failed_count = 0

    for png_path in tqdm(files_to_convert, desc="🔄 Конвертация PNG → WEBP", unit="файл"):
        success = save_webp_with_metadata(png_path)
        if success:
            converted_count += 1
        else:
            failed_count += 1

    print(f"\n✅ Готово! Успешно: {converted_count}, Ошибки: {failed_count}")

    # Пауза перед закрытием окна
    input("\nНажмите Enter, чтобы закрыть окно...")


if __name__ == "__main__":
    main()
