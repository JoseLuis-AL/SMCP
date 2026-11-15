// Generates the application icon files from the SVG artwork.
//
//   SMCP_make_app_icon <icon.svg> <output directory>
//
// Writes app-icon.ico (16 to 256 pixels, for the executable) and app-icon.png (256 pixels, for
// window title bars). The artwork uses a text element, so it must be rendered on a machine that
// has its font installed; the generated files are committed so builds and users do not need it.
// Run it again only when the artwork changes.

#include <iostream>

#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QTemporaryFile>
#include <QIcon>
#include <QImage>
#include <QPainter>

namespace
{
	/// One image of an .ico file as a device-independent bitmap: header, BGRA rows bottom-up, 1-bit mask.
	QByteArray IconBitmap(const QImage& source)
	{
		const QImage image = source.convertToFormat(QImage::Format_ARGB32);
		const int size = image.width();
		QByteArray data;
		QDataStream out(&data, QIODevice::WriteOnly);
		out.setByteOrder(QDataStream::LittleEndian);
		out << quint32(40) << qint32(size) << qint32(2 * size) << quint16(1) << quint16(32) << quint32(0)
			<< quint32(0) << qint32(0) << qint32(0) << quint32(0) << quint32(0);
		for (int y = size - 1; y >= 0; --y)
		{
			out.writeRawData(reinterpret_cast<const char*>(image.constScanLine(y)), size * 4);
		}
		// AND mask, unused with an alpha channel but required by the format; rows padded to 32 bits.
		const QByteArray maskRow(((size + 31) / 32) * 4, '\0');
		for (int y = 0; y < size; ++y)
		{
			out.writeRawData(maskRow.constData(), maskRow.size());
		}
		return data;
	}
} // namespace

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	if (argc < 3)
	{
		std::cout << "usage: SMCP_make_app_icon <icon.svg> <output directory>" << std::endl;
		return 2;
	}
	const QString source = QString::fromLocal8Bit(argv[1]);
	const QDir output(QString::fromLocal8Bit(argv[2]));

	// The artwork gives its font as a list ('AbadiMT-ExtraBold', sans-serif). Qt's SVG renderer does
	// not resolve such a list and silently draws the text with a default font, so the list is
	// reduced to the first family, and the tool stops if that font is not available.
	QFile artwork(source);
	if (!artwork.open(QIODevice::ReadOnly))
	{
		std::cout << "cannot read " << source.toStdString() << std::endl;
		return 1;
	}
	QString text = QString::fromUtf8(artwork.readAll());
	const QRegularExpression fontFamily("font-family:'([^']+)'[^;]*;");
	const QRegularExpressionMatch match = fontFamily.match(text);
	if (match.hasMatch())
	{
		const QString family = match.captured(1);
		// Fonts installed for the current user only are not always listed by Qt: load the file itself.
		for (const QString& directory : { QString::fromLocal8Bit(qgetenv("LOCALAPPDATA")) + "/Microsoft/Windows/Fonts",
				 QString::fromLocal8Bit(qgetenv("WINDIR")) + "/Fonts" })
		{
			if (QFile::exists(directory + "/" + family + ".ttf"))
				QFontDatabase::addApplicationFont(directory + "/" + family + ".ttf");
		}
		if (!QFontDatabase().families().contains(family))
		{
			std::cout << "the font of the artwork (" << family.toStdString() << ") is not installed" << std::endl;
			return 1;
		}
		std::cout << "font: " << family.toStdString() << std::endl;
		text.replace(match.captured(0), QString("font-family:'%1';").arg(family));
	}
	QTemporaryFile patched(QDir::temp().filePath("smcp-icon-XXXXXX.svg"));
	if (!patched.open() || patched.write(text.toUtf8()) < 0)
	{
		std::cout << "cannot write a temporary file" << std::endl;
		return 1;
	}
	patched.close();
	const QString svg = patched.fileName();
	const QIcon icon(svg);
	if (icon.isNull() || icon.pixmap(256, 256).isNull())
	{
		std::cout << "cannot render " << svg.toStdString() << std::endl;
		return 1;
	}

	const auto render = [&icon](int size)
	{
		QImage image(size, size, QImage::Format_ARGB32);
		image.fill(Qt::transparent);
		QPainter painter(&image);
		painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
		icon.paint(&painter, QRect(0, 0, size, size));
		return image;
	};

	const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 128, 256 };
	QList<QByteArray> images;
	for (int size : sizes)
	{
		const QImage image = render(size);
		if (size == 256)
		{
			// Large entries are stored as PNG, as Windows expects.
			QByteArray png;
			QBuffer buffer(&png);
			buffer.open(QIODevice::WriteOnly);
			image.save(&buffer, "PNG");
			images << png;
			if (!image.save(output.filePath("app-icon.png")))
			{
				std::cout << "cannot write app-icon.png" << std::endl;
				return 1;
			}
		}
		else
		{
			images << IconBitmap(image);
		}
	}

	QFile file(output.filePath("app-icon.ico"));
	if (!file.open(QIODevice::WriteOnly))
	{
		std::cout << "cannot write app-icon.ico" << std::endl;
		return 1;
	}
	QDataStream out(&file);
	out.setByteOrder(QDataStream::LittleEndian);
	const int count = static_cast<int>(sizeof(sizes) / sizeof(sizes[0]));
	out << quint16(0) << quint16(1) << quint16(count);
	quint32 offset = 6 + 16 * count;
	for (int i = 0; i < count; ++i)
	{
		const int size = sizes[i];
		out << quint8(size == 256 ? 0 : size) << quint8(size == 256 ? 0 : size) << quint8(0) << quint8(0) << quint16(1)
			<< quint16(32) << quint32(images[i].size()) << offset;
		offset += static_cast<quint32>(images[i].size());
	}
	for (const QByteArray& image : images)
	{
		out.writeRawData(image.constData(), image.size());
	}
	std::cout << "wrote " << file.fileName().toStdString() << " and app-icon.png" << std::endl;
	return 0;
}
