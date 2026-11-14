#include <QtTest/QtTest>

#include <cstdio>
#include <memory>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QTemporaryDir>

#include "Common.h"
#include "app/Branding.h"

// 品牌定制单元测试：只用临时目录与合成的 SVG/PNG 素材，
// 通过 KYLINLIST_BRANDING_DIR / KYLINLIST_DATA_DIR 隔离，不接触
// 真实用户配置，也不访问网络。QIcon/QPixmap 需要 QApplication，
// 因此用 QTEST_MAIN 并链接 Qt5::Widgets。

using namespace kl;

namespace {

const QString kManifestName = QStringLiteral("branding.json");

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write(bytes);
    file.close();
    return file.error() == QFile::NoError;
}

bool writeManifest(const QString &dir, const QString &json)
{
    if (!QDir().mkpath(dir))
        return false;
    return writeFile(QDir(dir).filePath(kManifestName), json.toUtf8());
}

bool writeSvgLogo(const QString &path)
{
    const QByteArray svg = QByteArrayLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"32\" height=\"32\""
        " viewBox=\"0 0 32 32\"><rect x=\"2\" y=\"2\" width=\"28\" height=\"28\""
        " fill=\"#00594f\"/></svg>");
    return writeFile(path, svg);
}

bool writePngLogo(const QString &path)
{
    QImage image(48, 48, QImage::Format_ARGB32);
    image.fill(Qt::darkGreen);
    return image.save(path, "png");
}

// iconengines/qsvgicon 与 imageformats/qsvg 同属 QtSvg 组件；
// 用 QImageReader 探测 svg 支持，插件缺失的环境跳过 SVG 用例。
bool svgSupportAvailable()
{
    return QImageReader::supportedImageFormats().contains(QByteArrayLiteral("svg"));
}

} // namespace

class BrandingTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void noConfigUsesDefaults();
    void envDirWinsOverDataDir();
    void displayNameOnly();
    void logoOnly();
    void absoluteLogoPath();
    void brokenJsonFallsBack();
    void notAnObjectFallsBack();
    void emptyObjectKeepsDefaults();
    void brokenImageKeepsDisplayName();
    void missingLogoFileFallsBack();
    void invalidValuesFallBack();
    void dataDirUsedWhenEnvMissing();
    void repeatedLoadRefreshesState();
    void identityDoesNotFollowDisplayName();

private:
    QString envBrandingDir() const { return m_brandingDir->path(); }
    QString dataBrandingDir() const { return m_dataDir->path() + QStringLiteral("/branding"); }

    std::unique_ptr<QTemporaryDir> m_dataDir;
    std::unique_ptr<QTemporaryDir> m_brandingDir;
};

void BrandingTest::initTestCase()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // CTest 已通过环境变量设置 offscreen；手动运行时兜底。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
}

void BrandingTest::init()
{
    m_dataDir.reset(new QTemporaryDir);
    m_brandingDir.reset(new QTemporaryDir);
    QVERIFY2(m_dataDir->isValid(), "临时数据目录创建失败");
    QVERIFY2(m_brandingDir->isValid(), "临时品牌目录创建失败");
    qputenv("KYLINLIST_DATA_DIR", m_dataDir->path().toUtf8());
    qputenv("KYLINLIST_BRANDING_DIR", m_brandingDir->path().toUtf8());
}

void BrandingTest::cleanup()
{
    qunsetenv("KYLINLIST_DATA_DIR");
    qunsetenv("KYLINLIST_BRANDING_DIR");
    m_dataDir.reset();
    m_brandingDir.reset();
}

void BrandingTest::noConfigUsesDefaults()
{
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(Branding::appIcon().isNull());
    QVERIFY(Branding::appIcon().isNull());  // 结果稳定，不因重复调用变化
    QVERIFY(Branding::configDirectory().isEmpty());
}

void BrandingTest::envDirWinsOverDataDir()
{
    if (!svgSupportAvailable())
        QSKIP("当前环境缺少 QtSvg 插件，跳过 SVG 品牌用例");
    QVERIFY(writeSvgLogo(QDir(envBrandingDir()).filePath(QStringLiteral("logo.svg"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"单位办事实务清单\",\"logo\":\"logo.svg\"}")));
    // 数据目录放一份更靠后的配置，验证环境变量优先级。
    QVERIFY(writeManifest(dataBrandingDir(), QStringLiteral("{\"displayName\":\"不应该生效\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("单位办事实务清单"));
    QVERIFY(!Branding::appIcon().isNull());
    QCOMPARE(Branding::configDirectory(), QDir(envBrandingDir()).absolutePath());
}

void BrandingTest::displayNameOnly()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"麒麟工作台\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟工作台"));
    QVERIFY(Branding::appIcon().isNull());
    QCOMPARE(Branding::configDirectory(), QDir(envBrandingDir()).absolutePath());
}

void BrandingTest::logoOnly()
{
    // 相对路径按配置目录解析，允许嵌套子目录。
    const QString nested = envBrandingDir() + QStringLiteral("/assets/icons");
    QVERIFY(QDir().mkpath(nested));
    QVERIFY(writePngLogo(nested + QStringLiteral("/mark.png")));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"assets/icons/mark.png\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(!Branding::appIcon().isNull());
    QCOMPARE(Branding::configDirectory(), QDir(envBrandingDir()).absolutePath());
}

void BrandingTest::absoluteLogoPath()
{
    // logo 允许指向配置目录之外的绝对路径（这里放在隔离的数据目录里）。
    const QString external = m_dataDir->path() + QStringLiteral("/unit-mark.png");
    QVERIFY(writePngLogo(external));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"单位台账清单\",\"logo\":\"%1\"}").arg(external)));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("单位台账清单"));
    QVERIFY(!Branding::appIcon().isNull());
}

void BrandingTest::brokenJsonFallsBack()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{ 这不是合法JSON ")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(Branding::appIcon().isNull());
    // 已命中目录但内容损坏：目录仍记录，值全部回退默认。
    QCOMPARE(Branding::configDirectory(), QDir(envBrandingDir()).absolutePath());
}

void BrandingTest::notAnObjectFallsBack()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("[1,2,3]")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(Branding::appIcon().isNull());
    QCOMPARE(Branding::configDirectory(), QDir(envBrandingDir()).absolutePath());
}

void BrandingTest::emptyObjectKeepsDefaults()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(Branding::appIcon().isNull());
    QCOMPARE(Branding::configDirectory(), QDir(envBrandingDir()).absolutePath());
}

void BrandingTest::brokenImageKeepsDisplayName()
{
    QVERIFY(writeFile(QDir(envBrandingDir()).filePath(QStringLiteral("bad.png")),
                      QByteArrayLiteral("这只是文本，不是PNG数据")));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"名称不受影响\",\"logo\":\"bad.png\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("名称不受影响"));
    QVERIFY(Branding::appIcon().isNull());
}

void BrandingTest::missingLogoFileFallsBack()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"no-such-file.png\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(Branding::appIcon().isNull());
}

void BrandingTest::invalidValuesFallBack()
{
    // displayName: 空白 / 超长 / 非字符串，均保持默认。
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"   \"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));

    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"%1\"}").arg(QStringLiteral("长").repeated(81))));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));

    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":42}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));

    // 恰好 80 个字符应当接受。
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"%1\"}").arg(QStringLiteral("名").repeated(80))));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("名").repeated(80));

    // logo: 空字符串 / 非字符串，忽略图标但保留合法的 displayName。
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"名称保留\",\"logo\":\"\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("名称保留"));
    QVERIFY(Branding::appIcon().isNull());

    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"名称保留\",\"logo\":7}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("名称保留"));
    QVERIFY(Branding::appIcon().isNull());
}

void BrandingTest::dataDirUsedWhenEnvMissing()
{
    // 环境变量未设置时回退到 <数据目录>/branding。
    qunsetenv("KYLINLIST_BRANDING_DIR");
    QVERIFY(writeManifest(dataBrandingDir(), QStringLiteral("{\"displayName\":\"数据目录品牌\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("数据目录品牌"));
    QVERIFY(Branding::appIcon().isNull());
    QCOMPARE(Branding::configDirectory(), QDir(dataBrandingDir()).absolutePath());
}

void BrandingTest::repeatedLoadRefreshesState()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"第一版\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("第一版"));
    QVERIFY(Branding::appIcon().isNull());

    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"第二版\",\"logo\":\"mark.png\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("第二版"));
    QVERIFY(!Branding::appIcon().isNull());

    // 配置消失后回到默认，图标缓存一并失效。
    QVERIFY(QFile::remove(QDir(envBrandingDir()).filePath(kManifestName)));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("麒麟清单"));
    QVERIFY(Branding::appIcon().isNull());
    QVERIFY(Branding::configDirectory().isEmpty());
}

void BrandingTest::identityDoesNotFollowDisplayName()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"完全不同的品牌\"}")));
    Branding::load();
    QCOMPARE(Branding::displayName(), QStringLiteral("完全不同的品牌"));
    // 内部产品标识与数据目录不随显示名改变。
    QCOMPARE(productId(), QStringLiteral("kylinlist"));
    QCOMPARE(productName(), QStringLiteral("KylinList"));
    QCOMPARE(productEnvironmentPrefix(), QStringLiteral("KYLINLIST"));
    QCOMPARE(dataDirPath(), QDir(m_dataDir->path()).absolutePath());
    QCOMPARE(databasePath(), QDir(m_dataDir->path()).absolutePath() + QStringLiteral("/kylinlist.db"));
}

QTEST_MAIN(BrandingTest)
#include "tst_branding.moc"
