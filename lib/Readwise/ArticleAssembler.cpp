#include "ArticleAssembler.h"

#include <cstdio>
#include <cstring>

#include "ZipWriter.h"

namespace readwise {
namespace {

constexpr const char* MIMETYPE = "application/epub+zip";

constexpr const char* CONTAINER_XML =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
    "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
    "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

// Images deliberately get no manifest entries. ChapterHtmlSlimParser resolves
// an <img src> against contentBase and reads the zip entry directly
// (ChapterHtmlSlimParser.cpp:619) rather than going through the OPF, so
// listing them would buy nothing and would have to be deferred until after
// every extension was known.
constexpr const char* OPF_HEAD =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\">"
    "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
    "<dc:identifier id=\"bookid\">";
constexpr const char* OPF_AFTER_ID = "</dc:identifier><dc:language>en</dc:language><dc:title>";
constexpr const char* OPF_AFTER_TITLE = "</dc:title><dc:creator>";
constexpr const char* OPF_TAIL =
    "</dc:creator></metadata>"
    "<manifest>"
    "<item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/>"
    "</manifest>"
    "<spine><itemref idref=\"article\"/></spine></package>";

// The magic bytes that settle what a downloaded image really is. The URL's
// extension is not evidence: many image URLs have none, and some lie.
enum class ImageFormat : uint8_t { Unknown, Jpeg, Png };

ImageFormat sniffFormat(ReadwiseFileStore& store, const std::string& path) {
  uint8_t header[8] = {};
  const int read = store.readRange(path, 0, header, sizeof(header));
  if (read < 8) {
    return ImageFormat::Unknown;
  }
  if (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return ImageFormat::Jpeg;
  }
  static constexpr uint8_t PNG_MAGIC[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(header, PNG_MAGIC, sizeof(PNG_MAGIC)) == 0) {
    return ImageFormat::Png;
  }
  return ImageFormat::Unknown;
}

// Escapes text for XML element content. The OPF is parsed by expat too, so a
// title containing '&' would break the package the same way it would break a
// chapter.
void appendEscaped(std::string& out, const char* text) {
  if (text == nullptr) {
    return;
  }
  for (const char* p = text; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
      continue;
    }
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += static_cast<char>(c);
        break;
    }
  }
}

std::string buildOpf(const char* identifier, const char* title, const char* author) {
  std::string opf;
  opf.reserve(512);
  opf += OPF_HEAD;
  appendEscaped(opf, identifier);
  opf += OPF_AFTER_ID;
  appendEscaped(opf, title);
  opf += OPF_AFTER_TITLE;
  appendEscaped(opf, author);
  opf += OPF_TAIL;
  return opf;
}

}  // namespace

ArticleAssemblyResult assembleArticleEpub(ReadwiseFileStore& store, ArticleImageFetcher& fetcher,
                                          const ArticleBodyWriter& body, const std::string& xhtmlPath,
                                          const std::string& scratchPath, const std::string& archivePath,
                                          const char* title, const char* author, ArticleImageProgress progress,
                                          void* progressCtx) {
  ArticleAssemblyResult result;
  result.imagesRequested = body.imageCount();

  if (store.size(xhtmlPath) < 0) {
    return result;
  }

  // Downloaded ahead of the archive so the extension patches below land in the
  // XHTML before it is folded in. Each image reuses one scratch slot, so peak
  // extra SD is a single image rather than the whole set.
  size_t totalBytes = 0;
  size_t stored = 0;
  bool formatIsPng[MAX_ARTICLE_IMAGES] = {};
  bool present[MAX_ARTICLE_IMAGES] = {};
  const size_t count = body.imageCount() < MAX_ARTICLE_IMAGES ? body.imageCount() : MAX_ARTICLE_IMAGES;

  ZipWriter zip(store);
  if (!zip.begin(archivePath)) {
    return result;
  }
  // The spec wants mimetype first and stored; ZipFile is central-directory
  // driven and would not care, but desktop tools do.
  if (!zip.addEntry("mimetype", std::string(MIMETYPE)) ||
      !zip.addEntry("META-INF/container.xml", std::string(CONTAINER_XML))) {
    zip.abort();
    return result;
  }

  for (size_t i = 0; i < count; ++i) {
    if (progress != nullptr) {
      progress(progressCtx, i, count);
    }
    const ArticleImageRef image = body.image(i);
    if (image.url[0] == '\0' || totalBytes >= MAX_TOTAL_IMAGE_BYTES) {
      continue;
    }

    const size_t remaining = MAX_TOTAL_IMAGE_BYTES - totalBytes;
    const size_t budget = remaining < MAX_IMAGE_BYTES ? remaining : MAX_IMAGE_BYTES;
    if (!fetcher.fetch(image.url, scratchPath, budget)) {
      continue;  // dangling src -> "[Image: alt]" in the reader
    }

    const ImageFormat format = sniffFormat(store, scratchPath);
    if (format == ImageFormat::Unknown) {
      // The URL promised an image and delivered something else -- an HTML
      // error page, a WebP, a redirect body. Storing it would only fail later
      // in the decoder.
      store.remove(scratchPath);
      continue;
    }

    const long size = store.size(scratchPath);
    if (size <= 0) {
      store.remove(scratchPath);
      continue;
    }

    char entryName[32];
    snprintf(entryName, sizeof(entryName), "OEBPS/images/%u.%s", static_cast<unsigned>(i),
             format == ImageFormat::Png ? "png" : "jpg");
    if (!zip.addEntryFromFile(entryName, scratchPath)) {
      // A half-written entry cannot be undone, so the archive is finished.
      zip.abort();
      store.remove(scratchPath);
      return result;
    }
    store.remove(scratchPath);

    formatIsPng[i] = format == ImageFormat::Png;
    present[i] = true;
    totalBytes += static_cast<size_t>(size);
    ++stored;
  }

  // '.jpg' was provisional. Both spellings are three characters, so correcting
  // it is a fixed-size overwrite rather than a rewrite of the document.
  for (size_t i = 0; i < count; ++i) {
    if (!present[i] || !formatIsPng[i]) {
      continue;
    }
    const ArticleImageRef image = body.image(i);
    if (!store.writeRange(xhtmlPath, image.extensionOffset, reinterpret_cast<const uint8_t*>("png"), 3)) {
      zip.abort();
      return result;
    }
  }

  if (!zip.addEntryFromFile("OEBPS/article.xhtml", xhtmlPath) ||
      !zip.addEntry("OEBPS/content.opf", buildOpf(archivePath.c_str(), title, author)) || !zip.finish()) {
    zip.abort();
    return result;
  }

  store.remove(xhtmlPath);
  if (progress != nullptr) {
    progress(progressCtx, count, count);
  }
  result.ok = true;
  result.imagesStored = stored;
  return result;
}

}  // namespace readwise
