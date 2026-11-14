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
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "Common.h"
#include "app/Branding.h"
#include "platform/DesktopBranding.h"

// 品牌定制单元测试：只用临时目录与合成的 SVG/PNG 素材，
// 通过 KYLINLIST_BRANDING_DIR / KYLINLIST_DATA_DIR 隔离，不接触
// 真实用户配置，也不访问网络。QIcon/QPixmap 需要 QApplication，
// 因此用 QTEST_MAIN 并链接 Qt5::Widgets。
//
// DesktopBranding（安装阶段系统桌面覆盖）同样只落在临时目录：
// KYLINLIST_DESKTOP_BRANDING_ROOT / KYLINLIST_DESKTOP_ENTRY_SOURCE
// 都在 init() 指向临时路径、cleanup() 恢复，绝不写真实 /usr/local
// 或 /usr/share/applications。

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

// 与 packaging/KylinList.desktop 保持一致的测试源入口。
const QString kDefaultSourceDesktop = QStringLiteral(
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Version=1.0\n"
    "Name=KylinList\n"
    "Name[zh_CN]=麒麟清单\n"
    "GenericName=Task List\n"
    "GenericName[zh_CN]=任务清单\n"
    "Comment=Manage local tasks, checklists, schedules and reminders\n"
    "Comment[zh_CN]=管理本地任务、清单、日程与提醒，并可贴到桌面\n"
    "Exec=kylinlist\n"
    "TryExec=kylinlist\n"
    "Icon=kylinlist\n"
    "Terminal=false\n"
    "Categories=Utility;Office;\n"
    "Keywords=task;checklist;todo;note;sticky;任务;清单;待办;便签;\n"
    "StartupNotify=false\n");

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
    void brandingLogoPathTracksManifest();

    // DesktopBranding：安装阶段的系统桌面品牌覆盖。
    void desktopRefreshWithoutCustomizationKeepsDefault();
    void desktopRefreshWithNameAndSvgLogo();
    void desktopRefreshWithNameAndPngLogo();
    void desktopRefreshLogoOnlyKeepsName();
    void desktopRefreshBadLogoKeepsNameAndDefaultIcon();
    void desktopRefreshEscapesNameValue();
    void desktopRefreshTwiceIsIdempotent();
    void desktopRefreshSwitchesIconExtensionAndCleansStale();
    void desktopRefreshAfterLogoRemovalDropsIconOverride();
    void desktopRefreshAddsMissingKeys();
    void desktopRefreshOnlyTouchesEntryGroup();
    void desktopRefreshFallsBackWhenCustomizationGone();
    void desktopRefreshRefusesUnmanagedTarget();
    void desktopRefreshKeepsUnmanagedTargetWhenFallingBack();
    void desktopRefreshFailsWhenSourceMissing();
    void desktopClearRemovesManagedFilesOnly();
    void desktopClearKeepsUnmanagedDesktop();
    void desktopClearOnEmptyRootSucceeds();

private:
    QString envBrandingDir() const { return m_brandingDir->path(); }
    QString dataBrandingDir() const { return m_dataDir->path() + QStringLiteral("/branding"); }

    QString desktopRoot() const { return m_desktopRoot->path(); }
    QString sourceDesktopPath() const
    { return QDir(m_sourceDesktopDir->path()).filePath(QStringLiteral("KylinList.desktop")); }
    QString targetDesktopPath() const
    { return desktopRoot() + QStringLiteral("/share/applications/KylinList.desktop"); }
    QString iconBrandingDir() const
    { return desktopRoot() + QStringLiteral("/share/kylinlist/branding"); }
    QString activeIconPath(const QString &extension) const
    { return iconBrandingDir() + QStringLiteral("/active.") + extension; }

    bool writeSourceDesktopContent(const QString &content);
    void writeDefaultSourceDesktop();
    bool readTargetDesktop(QString *content) const;
    QStringList listActiveIcons() const;
    QString desktopValue(const QString &content, const QString &key) const;
    bool placeUnmanagedTargetDesktop();

    std::unique_ptr<QTemporaryDir> m_dataDir;
    std::unique_ptr<QTemporaryDir> m_brandingDir;
    std::unique_ptr<QTemporaryDir> m_desktopRoot;
    std::unique_ptr<QTemporaryDir> m_sourceDesktopDir;
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
    m_desktopRoot.reset(new QTemporaryDir);
    m_sourceDesktopDir.reset(new QTemporaryDir);
    QVERIFY2(m_dataDir->isValid(), "临时数据目录创建失败");
    QVERIFY2(m_brandingDir->isValid(), "临时品牌目录创建失败");
    QVERIFY2(m_desktopRoot->isValid(), "临时桌面输出根目录创建失败");
    QVERIFY2(m_sourceDesktopDir->isValid(), "临时源 desktop 目录创建失败");
    qputenv("KYLINLIST_DATA_DIR", m_dataDir->path().toUtf8());
    qputenv("KYLINLIST_BRANDING_DIR", m_brandingDir->path().toUtf8());
    // DesktopBranding 全部重定向到临时目录，绝不写真实 /usr/local。
    qputenv("KYLINLIST_DESKTOP_BRANDING_ROOT", desktopRoot().toUtf8());
    writeDefaultSourceDesktop();
    qputenv("KYLINLIST_DESKTOP_ENTRY_SOURCE", sourceDesktopPath().toUtf8());
}

void BrandingTest::cleanup()
{
    qunsetenv("KYLINLIST_DATA_DIR");
    qunsetenv("KYLINLIST_BRANDING_DIR");
    qunsetenv("KYLINLIST_DESKTOP_BRANDING_ROOT");
    qunsetenv("KYLINLIST_DESKTOP_ENTRY_SOURCE");
    m_dataDir.reset();
    m_brandingDir.reset();
    m_desktopRoot.reset();
    m_sourceDesktopDir.reset();
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

// ---- DesktopBranding 辅助 ----

bool BrandingTest::writeSourceDesktopContent(const QString &content)
{
    return writeFile(sourceDesktopPath(), content.toUtf8());
}

void BrandingTest::writeDefaultSourceDesktop()
{
    QVERIFY2(writeSourceDesktopContent(kDefaultSourceDesktop), "写入测试源 desktop 失败");
}

bool BrandingTest::readTargetDesktop(QString *content) const
{
    QFile file(targetDesktopPath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    *content = QString::fromUtf8(file.readAll());
    return true;
}

QStringList BrandingTest::listActiveIcons() const
{
    QDir dir(iconBrandingDir());
    if (!dir.exists()) return QStringList();
    return dir.entryList(QStringList() << QStringLiteral("active.*"), QDir::Files, QDir::Name);
}

// 返回目标 desktop [Desktop Entry] 组内 key= 的原始值（未做 desktop 反转义）。
QString BrandingTest::desktopValue(const QString &content, const QString &key) const
{
    const QString prefix = key + QLatin1Char('=');
    bool inEntry = false;
    const QStringList lines = content.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
            inEntry = trimmed == QStringLiteral("[Desktop Entry]");
            continue;
        }
        if (inEntry && line.startsWith(prefix)) return line.mid(prefix.size());
    }
    return QString();
}

// 在目标位置放置一份“管理员手工维护”的 desktop（无管理标记）。
bool BrandingTest::placeUnmanagedTargetDesktop()
{
    const QString path = targetDesktopPath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    return writeFile(path, QByteArrayLiteral("[Desktop Entry]\nName=管理员手工入口\nExec=custom\n"));
}

// ---- Branding::logoPath 只读接口 ----

void BrandingTest::brandingLogoPathTracksManifest()
{
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"仅名称\"}")));
    Branding::load();
    QVERIFY(Branding::logoPath().isEmpty());  // 未配置 logo 为空

    const QString nested = envBrandingDir() + QStringLiteral("/assets/icons");
    QVERIFY(QDir().mkpath(nested));
    QVERIFY(writePngLogo(nested + QStringLiteral("/mark.png")));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"assets/icons/mark.png\"}")));
    Branding::load();
    QCOMPARE(Branding::logoPath(),
             QDir(envBrandingDir()).absolutePath() + QStringLiteral("/assets/icons/mark.png"));

    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"名称保留\",\"logo\":\"\"}")));
    Branding::load();
    QVERIFY(Branding::logoPath().isEmpty());  // 无效 logo 不留下旧路径
}

// ---- DesktopBranding：refreshSystem ----

void BrandingTest::desktopRefreshWithoutCustomizationKeepsDefault()
{
    // init 未写入 manifest：无自定义时不生成任何 override。
    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetDesktopPath()));
    QVERIFY(!QFileInfo::exists(iconBrandingDir()));
    QVERIFY(!QFileInfo::exists(desktopRoot() + QStringLiteral("/share")));
}

void BrandingTest::desktopRefreshWithNameAndSvgLogo()
{
    if (!svgSupportAvailable())
        QSKIP("当前环境缺少 QtSvg 插件，跳过 SVG 品牌用例");
    QVERIFY(writeSvgLogo(QDir(envBrandingDir()).filePath(QStringLiteral("logo.svg"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"单位办事实务清单\",\"logo\":\"logo.svg\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("单位办事实务清单"));
    QCOMPARE(desktopValue(content, QStringLiteral("Name[zh_CN]")), QStringLiteral("单位办事实务清单"));
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), activeIconPath(QStringLiteral("svg")));
    QCOMPARE(desktopValue(content, QStringLiteral("X-KylinList-Managed-Branding")),
             QStringLiteral("true"));
    // Exec/TryExec 等无关键保持源文件内容。
    QCOMPARE(desktopValue(content, QStringLiteral("Exec")), QStringLiteral("kylinlist"));
    QCOMPARE(desktopValue(content, QStringLiteral("TryExec")), QStringLiteral("kylinlist"));
    QCOMPARE(desktopValue(content, QStringLiteral("GenericName")), QStringLiteral("Task List"));
    QCOMPARE(desktopValue(content, QStringLiteral("GenericName[zh_CN]")), QStringLiteral("任务清单"));

    QCOMPARE(listActiveIcons(), QStringList() << QStringLiteral("active.svg"));
    QVERIFY2(QFile::exists(activeIconPath(QStringLiteral("svg"))), "active.svg 未落盘");
    QVERIFY(!QImageReader(activeIconPath(QStringLiteral("svg"))).read().isNull());
}

void BrandingTest::desktopRefreshWithNameAndPngLogo()
{
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"单位台账清单\",\"logo\":\"mark.png\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("单位台账清单"));
    QCOMPARE(desktopValue(content, QStringLiteral("Name[zh_CN]")), QStringLiteral("单位台账清单"));
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), activeIconPath(QStringLiteral("png")));
    QCOMPARE(desktopValue(content, QStringLiteral("X-KylinList-Managed-Branding")),
             QStringLiteral("true"));
    QCOMPARE(listActiveIcons(), QStringList() << QStringLiteral("active.png"));
    QVERIFY(!QImageReader(activeIconPath(QStringLiteral("png"))).read().isNull());
}

void BrandingTest::desktopRefreshLogoOnlyKeepsName()
{
    // 只自定义 logo：Name 保持源文件默认，Icon 换成绝对路径。
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"logo\":\"mark.png\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("KylinList"));
    QCOMPARE(desktopValue(content, QStringLiteral("Name[zh_CN]")), QStringLiteral("麒麟清单"));
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), activeIconPath(QStringLiteral("png")));
    QCOMPARE(desktopValue(content, QStringLiteral("X-KylinList-Managed-Branding")),
             QStringLiteral("true"));
}

void BrandingTest::desktopRefreshBadLogoKeepsNameAndDefaultIcon()
{
    // 坏图：只回退图标，名称照常生效，Icon 保持源默认值，不落 active 图标。
    QVERIFY(writeFile(QDir(envBrandingDir()).filePath(QStringLiteral("bad.png")),
                      QByteArrayLiteral("这只是文本，不是PNG数据")));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"坏图名称\",\"logo\":\"bad.png\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("坏图名称"));
    QCOMPARE(desktopValue(content, QStringLiteral("Name[zh_CN]")), QStringLiteral("坏图名称"));
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), QStringLiteral("kylinlist"));
    QCOMPARE(desktopValue(content, QStringLiteral("X-KylinList-Managed-Branding")),
             QStringLiteral("true"));
    QVERIFY(listActiveIcons().isEmpty());
}

void BrandingTest::desktopRefreshEscapesNameValue()
{
    // 显示名包含反斜杠、换行、回车、制表：desktop 值必须转义为
    // \\、\n、\r、\t 字面序列，不能把文件撕开多行。
    const QString tricky = QStringLiteral("甲\\乙\n丙\t丁\r戊");
    QJsonObject manifest;
    manifest.insert(QStringLiteral("displayName"), tricky);
    QVERIFY(writeManifest(envBrandingDir(),
                          QString::fromUtf8(QJsonDocument(manifest).toJson(QJsonDocument::Compact))));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    const QString expected = QStringLiteral("甲\\\\乙\\n丙\\t丁\\r戊");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), expected);
    QCOMPARE(desktopValue(content, QStringLiteral("Name[zh_CN]")), expected);
}

void BrandingTest::desktopRefreshTwiceIsIdempotent()
{
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"幂等名称\",\"logo\":\"mark.png\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QString first;
    QVERIFY2(readTargetDesktop(&first), "目标 desktop 未生成");

    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QString second;
    QVERIFY2(readTargetDesktop(&second), "目标 desktop 未生成");
    QCOMPARE(second, first);  // 重复执行结果一致
    QCOMPARE(listActiveIcons(), QStringList() << QStringLiteral("active.png"));
}

void BrandingTest::desktopRefreshSwitchesIconExtensionAndCleansStale()
{
    if (!svgSupportAvailable())
        QSKIP("当前环境缺少 QtSvg 插件，跳过 SVG 切换用例");
    QVERIFY(writeSvgLogo(QDir(envBrandingDir()).filePath(QStringLiteral("logo.svg"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"切图名称\",\"logo\":\"logo.svg\"}")));
    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), activeIconPath(QStringLiteral("svg")));

    // 换成 PNG：旧扩展名的 active.svg 必须清理，Icon 同步更新。
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"切图名称\",\"logo\":\"mark.png\"}")));
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), activeIconPath(QStringLiteral("png")));
    QCOMPARE(listActiveIcons(), QStringList() << QStringLiteral("active.png"));
    QVERIFY(!QFile::exists(activeIconPath(QStringLiteral("svg"))));
}

void BrandingTest::desktopRefreshAfterLogoRemovalDropsIconOverride()
{
    // 先有名称+图标，再改为仅名称：active 图标清理，Icon 回退源默认值。
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"仅名称\",\"logo\":\"mark.png\"}")));
    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"仅名称\"}")));
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("仅名称"));
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), QStringLiteral("kylinlist"));
    QCOMPARE(desktopValue(content, QStringLiteral("X-KylinList-Managed-Branding")),
             QStringLiteral("true"));
    QVERIFY(listActiveIcons().isEmpty());
}

void BrandingTest::desktopRefreshAddsMissingKeys()
{
    // 源 desktop 缺 Name/Name[zh_CN]/Icon：有自定义时补齐键而不是放弃。
    QVERIFY(writeSourceDesktopContent(QStringLiteral(
        "[Desktop Entry]\nType=Application\nExec=kylinlist\nTerminal=false\n")));
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"补齐名称\",\"logo\":\"mark.png\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("补齐名称"));
    QCOMPARE(desktopValue(content, QStringLiteral("Name[zh_CN]")), QStringLiteral("补齐名称"));
    QCOMPARE(desktopValue(content, QStringLiteral("Icon")), activeIconPath(QStringLiteral("png")));
    QCOMPARE(desktopValue(content, QStringLiteral("Exec")), QStringLiteral("kylinlist"));
    QCOMPARE(desktopValue(content, QStringLiteral("X-KylinList-Managed-Branding")),
             QStringLiteral("true"));
}

void BrandingTest::desktopRefreshOnlyTouchesEntryGroup()
{
    // [Desktop Action] 组里的 Name/Exec 不允许被改写。
    QVERIFY(writeSourceDesktopContent(kDefaultSourceDesktop + QStringLiteral(
        "\n[Desktop Action new-task]\nName=New Task\nName[zh_CN]=新建任务\nExec=kylinlist --new\n")));
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"主组名称\"}")));

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "目标 desktop 未生成");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("主组名称"));
    QVERIFY(content.contains(QStringLiteral("\nName=New Task\n")));
    QVERIFY(content.contains(QStringLiteral("Name[zh_CN]=新建任务\n")));
    QVERIFY(content.contains(QStringLiteral("Exec=kylinlist --new\n")));
}

void BrandingTest::desktopRefreshFallsBackWhenCustomizationGone()
{
    // 有自定义时生成 override；配置消失后再次 refresh 应回退默认并清理。
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"临时品牌\",\"logo\":\"mark.png\"}")));
    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QVERIFY(QFileInfo::exists(targetDesktopPath()));

    QVERIFY(QFile::remove(QDir(envBrandingDir()).filePath(kManifestName)));
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetDesktopPath()));  // 删除的是自己管理的 override
    QVERIFY(listActiveIcons().isEmpty());
}

void BrandingTest::desktopRefreshRefusesUnmanagedTarget()
{
    // 目标位置已有非本程序管理的 desktop：拒绝覆盖并报中文错误。
    QVERIFY2(placeUnmanagedTargetDesktop(), "放置非 managed 目标失败");
    const QByteArray original = [&]() {
        QFile file(targetDesktopPath());
        file.open(QIODevice::ReadOnly);
        return file.readAll();
    }();

    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"覆盖尝试\"}")));
    QString error;
    QVERIFY2(!DesktopBranding::refreshSystem(&error), "应当拒绝覆盖非 managed 目标");
    QVERIFY2(error.contains(QStringLiteral("拒绝")), qPrintable(error));

    QFile file(targetDesktopPath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), original);  // 文件内容原样保留
    QVERIFY(listActiveIcons().isEmpty());
}

void BrandingTest::desktopRefreshKeepsUnmanagedTargetWhenFallingBack()
{
    // 无自定义且回退默认时，也不能碰管理员手工文件。
    QVERIFY2(placeUnmanagedTargetDesktop(), "放置非 managed 目标失败");

    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    QString content;
    QVERIFY2(readTargetDesktop(&content), "非 managed 目标被误删");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("管理员手工入口"));
}

void BrandingTest::desktopRefreshFailsWhenSourceMissing()
{
    // 找不到源 desktop：报错退出，且不产生任何输出。
    qputenv("KYLINLIST_DESKTOP_ENTRY_SOURCE",
            (m_sourceDesktopDir->path() + QStringLiteral("/no-such.desktop")).toUtf8());
    QVERIFY(writeManifest(envBrandingDir(), QStringLiteral("{\"displayName\":\"无源名称\"}")));

    QString error;
    QVERIFY2(!DesktopBranding::refreshSystem(&error), "源 desktop 缺失应当失败");
    QVERIFY2(error.contains(QStringLiteral("源 desktop")), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetDesktopPath()));
    QVERIFY(!QFileInfo::exists(desktopRoot() + QStringLiteral("/share")));
}

// ---- DesktopBranding：clearSystem ----

void BrandingTest::desktopClearRemovesManagedFilesOnly()
{
    QVERIFY(writePngLogo(QDir(envBrandingDir()).filePath(QStringLiteral("mark.png"))));
    QVERIFY(writeManifest(envBrandingDir(),
                          QStringLiteral("{\"displayName\":\"清理对象\",\"logo\":\"mark.png\"}")));
    QString error;
    QVERIFY2(DesktopBranding::refreshSystem(&error), qPrintable(error));
    // 品牌目录里混入非 active.* 的文件：clear 不得误删。
    QVERIFY(QDir().mkpath(iconBrandingDir()));
    QVERIFY(writeFile(iconBrandingDir() + QStringLiteral("/keep.txt"), QByteArrayLiteral("keep")));
    QVERIFY(writeFile(iconBrandingDir() + QStringLiteral("/other.png"), QByteArrayLiteral("png")));

    QVERIFY2(DesktopBranding::clearSystem(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetDesktopPath()));  // managed desktop 已删
    QVERIFY(listActiveIcons().isEmpty());              // active 图标已删
    QVERIFY(QFileInfo::exists(iconBrandingDir() + QStringLiteral("/keep.txt")));
    QVERIFY(QFileInfo::exists(iconBrandingDir() + QStringLiteral("/other.png")));

    // 重复清理结果一致。
    QVERIFY2(DesktopBranding::clearSystem(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetDesktopPath()));
}

void BrandingTest::desktopClearKeepsUnmanagedDesktop()
{
    QVERIFY2(placeUnmanagedTargetDesktop(), "放置非 managed 目标失败");
    QVERIFY(QDir().mkpath(iconBrandingDir()));
    QVERIFY(writePngLogo(activeIconPath(QStringLiteral("png"))));

    QString error;
    QVERIFY2(DesktopBranding::clearSystem(&error), qPrintable(error));

    QString content;
    QVERIFY2(readTargetDesktop(&content), "非 managed 目标被误删");
    QCOMPARE(desktopValue(content, QStringLiteral("Name")), QStringLiteral("管理员手工入口"));
    QVERIFY(listActiveIcons().isEmpty());  // active 图标仍属本模块，照常清理
}

void BrandingTest::desktopClearOnEmptyRootSucceeds()
{
    // 从未生成过任何文件时清理也应成功（幂等）。
    QString error;
    QVERIFY2(DesktopBranding::clearSystem(&error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(targetDesktopPath()));
    QVERIFY(!QFileInfo::exists(iconBrandingDir()));
}

QTEST_MAIN(BrandingTest)
#include "tst_branding.moc"
