#include "messages/MessageBuilder.hpp"

#include "Application.hpp"
#include "common/LinkParser.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageColor.hpp"
#include "messages/MessageElement.hpp"
#include "providers/links/LinkResolver.hpp"
#include "providers/twitch/PubSubActions.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Emotes.hpp"
#include "util/FormatTime.hpp"

#include <QDateTime>

namespace {

QString formatUpdatedEmoteList(const QString &platform,
                               const std::vector<QString> &emoteNames,
                               bool isAdd, bool isFirstWord)
{
    QString text = "";
    if (isAdd)
    {
        text += isFirstWord ? "Added" : "added";
    }
    else
    {
        text += isFirstWord ? "Removed" : "removed";
    }

    if (emoteNames.size() == 1)
    {
        text += QString(" %1 emote ").arg(platform);
    }
    else
    {
        text += QString(" %1 %2 emotes ").arg(emoteNames.size()).arg(platform);
    }

    auto i = 0;
    for (const auto &emoteName : emoteNames)
    {
        i++;
        if (i > 1)
        {
            text += i == emoteNames.size() ? " and " : ", ";
        }
        text += emoteName;
    }

    text += ".";

    return text;
}

}  // namespace

namespace chatterino {

MessagePtr makeSystemMessage(const QString &text)
{
    return MessageBuilder(systemMessage, text).release();
}

MessagePtr makeSystemMessage(const QString &text, const QTime &time)
{
    return MessageBuilder(systemMessage, text, time).release();
}

MessageBuilder::MessageBuilder()
    : message_(std::make_shared<Message>())
{
}

MessageBuilder::MessageBuilder(SystemMessageTag, const QString &text,
                               const QTime &time)
    : MessageBuilder()
{
    this->emplace<TimestampElement>(time);

    // check system message for links
    // (e.g. needed for sub ticket message in sub only mode)
    const QStringList textFragments =
        text.split(QRegularExpression("\\s"), Qt::SkipEmptyParts);
    for (const auto &word : textFragments)
    {
        auto link = linkparser::parse(word);
        if (link)
        {
            this->addLink(*link, word);
            continue;
        }

        this->emplace<TextElement>(word, MessageElementFlag::Text,
                                   MessageColor::System);
    }
    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);
    this->message().messageText = text;
    this->message().searchText = text;
}

MessageBuilder::MessageBuilder(TimeoutMessageTag, const QString &timeoutUser,
                               const QString &sourceUser,
                               const QString &systemMessageText, int times,
                               const QTime &time)
    : MessageBuilder()
{
    QString usernameText = systemMessageText.split(" ").at(0);
    QString remainder = systemMessageText.mid(usernameText.length() + 1);
    bool timeoutUserIsFirst =
        usernameText == "You" || timeoutUser == usernameText;
    QString messageText;

    this->emplace<TimestampElement>(time);
    this->emplaceSystemTextAndUpdate(usernameText, messageText)
        ->setLink(
            {Link::UserInfo, timeoutUserIsFirst ? timeoutUser : sourceUser});

    if (!sourceUser.isEmpty())
    {
        // the second username in the message
        const auto &targetUsername =
            timeoutUserIsFirst ? sourceUser : timeoutUser;
        int userPos = remainder.indexOf(targetUsername);

        QString mid = remainder.mid(0, userPos - 1);
        QString username = remainder.mid(userPos, targetUsername.length());
        remainder = remainder.mid(userPos + targetUsername.length() + 1);

        this->emplaceSystemTextAndUpdate(mid, messageText);
        this->emplaceSystemTextAndUpdate(username, messageText)
            ->setLink({Link::UserInfo, username});
    }

    this->emplaceSystemTextAndUpdate(
        QString("%1 (%2 times)").arg(remainder.trimmed()).arg(times),
        messageText);

    this->message().messageText = messageText;
    this->message().searchText = messageText;
}

MessageBuilder::MessageBuilder(TimeoutMessageTag, const QString &username,
                               const QString &durationInSeconds,
                               bool multipleTimes, const QTime &time)
    : MessageBuilder()
{
    QString fullText;
    QString text;

    this->emplace<TimestampElement>(time);
    this->emplaceSystemTextAndUpdate(username, fullText)
        ->setLink({Link::UserInfo, username});

    if (!durationInSeconds.isEmpty())
    {
        text.append("has been timed out");

        // TODO: Implement who timed the user out

        text.append(" for ");
        bool ok = true;
        int timeoutSeconds = durationInSeconds.toInt(&ok);
        if (ok)
        {
            text.append(formatTime(timeoutSeconds));
        }
    }
    else
    {
        text.append("has been permanently banned");
    }

    text.append(".");

    if (multipleTimes)
    {
        text.append(" (multiple times)");
    }

    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::Timeout);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);
    this->message().timeoutUser = username;

    this->emplaceSystemTextAndUpdate(text, fullText);
    this->message().messageText = fullText;
    this->message().searchText = fullText;
}

// XXX: This does not belong in the MessageBuilder, this should be part of the TwitchMessageBuilder
MessageBuilder::MessageBuilder(const BanAction &action, uint32_t count)
    : MessageBuilder()
{
    auto current = getApp()->getAccounts()->twitch.getCurrent();

    this->emplace<TimestampElement>();
    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::Timeout);
    this->message().timeoutUser = action.target.login;
    this->message().loginName = action.source.login;
    this->message().count = count;

    QString text;

    if (action.target.id == current->getUserId())
    {
        this->emplaceSystemTextAndUpdate("You", text)
            ->setLink({Link::UserInfo, current->getUserName()});
        this->emplaceSystemTextAndUpdate("were", text);
        if (action.isBan())
        {
            this->emplaceSystemTextAndUpdate("banned", text);
        }
        else
        {
            this->emplaceSystemTextAndUpdate(
                QString("timed out for %1").arg(formatTime(action.duration)),
                text);
        }

        if (!action.source.login.isEmpty())
        {
            this->emplaceSystemTextAndUpdate("by", text);
            this->emplaceSystemTextAndUpdate(
                    action.source.login + (action.reason.isEmpty() ? "." : ":"),
                    text)
                ->setLink({Link::UserInfo, action.source.login});
        }

        if (!action.reason.isEmpty())
        {
            this->emplaceSystemTextAndUpdate(
                QString("\"%1\".").arg(action.reason), text);
        }
    }
    else
    {
        if (action.isBan())
        {
            this->emplaceSystemTextAndUpdate(action.source.login, text)
                ->setLink({Link::UserInfo, action.source.login});
            this->emplaceSystemTextAndUpdate("banned", text);
            if (action.reason.isEmpty())
            {
                this->emplaceSystemTextAndUpdate(action.target.login + ".",
                                                 text)
                    ->setLink({Link::UserInfo, action.target.login});
            }
            else
            {
                this->emplaceSystemTextAndUpdate(action.target.login + ":",
                                                 text)
                    ->setLink({Link::UserInfo, action.target.login});
                this->emplaceSystemTextAndUpdate(
                    QString("\"%1\".").arg(action.reason), text);
            }
        }
        else
        {
            this->emplaceSystemTextAndUpdate(action.source.login, text)
                ->setLink({Link::UserInfo, action.source.login});
            this->emplaceSystemTextAndUpdate("timed out", text);
            this->emplaceSystemTextAndUpdate(action.target.login, text)
                ->setLink({Link::UserInfo, action.target.login});
            if (action.reason.isEmpty())
            {
                this->emplaceSystemTextAndUpdate(
                    QString("for %1.").arg(formatTime(action.duration)), text);
            }
            else
            {
                this->emplaceSystemTextAndUpdate(
                    QString("for %1: \"%2\".")
                        .arg(formatTime(action.duration))
                        .arg(action.reason),
                    text);
            }

            if (count > 1)
            {
                this->emplaceSystemTextAndUpdate(
                    QString("(%1 times)").arg(count), text);
            }
        }
    }

    this->message().messageText = text;
    this->message().searchText = text;
}

MessageBuilder::MessageBuilder(const UnbanAction &action)
    : MessageBuilder()
{
    this->emplace<TimestampElement>();
    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::Untimeout);

    this->message().timeoutUser = action.target.login;

    QString text;

    this->emplaceSystemTextAndUpdate(action.source.login, text)
        ->setLink({Link::UserInfo, action.source.login});
    this->emplaceSystemTextAndUpdate(
        action.wasBan() ? "unbanned" : "untimedout", text);
    this->emplaceSystemTextAndUpdate(action.target.login + ".", text)
        ->setLink({Link::UserInfo, action.target.login});

    this->message().messageText = text;
    this->message().searchText = text;
}

MessageBuilder::MessageBuilder(const WarnAction &action)
    : MessageBuilder()
{
    this->emplace<TimestampElement>();
    this->message().flags.set(MessageFlag::System);

    QString text;

    // TODO: Use MentionElement here, once WarnAction includes username/displayname
    this->emplaceSystemTextAndUpdate("A moderator", text)
        ->setLink({Link::UserInfo, "id:" + action.source.id});
    this->emplaceSystemTextAndUpdate("warned", text);
    this->emplaceSystemTextAndUpdate(
            action.target.login + (action.reasons.isEmpty() ? "." : ":"), text)
        ->setLink({Link::UserInfo, action.target.login});

    if (!action.reasons.isEmpty())
    {
        this->emplaceSystemTextAndUpdate(action.reasons.join(", "), text);
    }

    this->message().messageText = text;
    this->message().searchText = text;
}

MessageBuilder::MessageBuilder(const AutomodUserAction &action)
    : MessageBuilder()
{
    this->emplace<TimestampElement>();
    this->message().flags.set(MessageFlag::System);

    QString text;
    switch (action.type)
    {
        case AutomodUserAction::AddPermitted: {
            text = QString("%1 added \"%2\" as a permitted term on AutoMod.")
                       .arg(action.source.login, action.message);
        }
        break;

        case AutomodUserAction::AddBlocked: {
            text = QString("%1 added \"%2\" as a blocked term on AutoMod.")
                       .arg(action.source.login, action.message);
        }
        break;

        case AutomodUserAction::RemovePermitted: {
            text = QString("%1 removed \"%2\" as a permitted term on AutoMod.")
                       .arg(action.source.login, action.message);
        }
        break;

        case AutomodUserAction::RemoveBlocked: {
            text = QString("%1 removed \"%2\" as a blocked term on AutoMod.")
                       .arg(action.source.login, action.message);
        }
        break;

        case AutomodUserAction::Properties: {
            text = QString("%1 modified the AutoMod properties.")
                       .arg(action.source.login);
        }
        break;
    }
    this->message().messageText = text;
    this->message().searchText = text;

    this->emplace<TextElement>(text, MessageElementFlag::Text,
                               MessageColor::System);
}

MessageBuilder::MessageBuilder(LiveUpdatesAddEmoteMessageTag /*unused*/,
                               const QString &platform, const QString &actor,
                               const std::vector<QString> &emoteNames)
    : MessageBuilder()
{
    auto text =
        formatUpdatedEmoteList(platform, emoteNames, true, actor.isEmpty());

    this->emplace<TimestampElement>();
    if (!actor.isEmpty())
    {
        this->emplace<TextElement>(actor, MessageElementFlag::Username,
                                   MessageColor::System)
            ->setLink({Link::UserInfo, actor});
    }
    this->emplace<TextElement>(text, MessageElementFlag::Text,
                               MessageColor::System);

    QString finalText;
    if (actor.isEmpty())
    {
        finalText = text;
    }
    else
    {
        finalText = QString("%1 %2").arg(actor, text);
    }

    this->message().loginName = actor;
    this->message().messageText = finalText;
    this->message().searchText = finalText;

    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::LiveUpdatesAdd);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);
}

MessageBuilder::MessageBuilder(LiveUpdatesRemoveEmoteMessageTag /*unused*/,
                               const QString &platform, const QString &actor,
                               const std::vector<QString> &emoteNames)
    : MessageBuilder()
{
    auto text =
        formatUpdatedEmoteList(platform, emoteNames, false, actor.isEmpty());

    this->emplace<TimestampElement>();
    if (!actor.isEmpty())
    {
        this->emplace<TextElement>(actor, MessageElementFlag::Username,
                                   MessageColor::System)
            ->setLink({Link::UserInfo, actor});
    }
    this->emplace<TextElement>(text, MessageElementFlag::Text,
                               MessageColor::System);

    QString finalText;
    if (actor.isEmpty())
    {
        finalText = text;
    }
    else
    {
        finalText = QString("%1 %2").arg(actor, text);
    }

    this->message().loginName = actor;
    this->message().messageText = finalText;
    this->message().searchText = finalText;

    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::LiveUpdatesRemove);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);
}

MessageBuilder::MessageBuilder(LiveUpdatesUpdateEmoteMessageTag /*unused*/,
                               const QString &platform, const QString &actor,
                               const QString &emoteName,
                               const QString &oldEmoteName)
    : MessageBuilder()
{
    QString text;
    if (actor.isEmpty())
    {
        text = "Renamed";
    }
    else
    {
        text = "renamed";
    }
    text +=
        QString(" %1 emote %2 to %3.").arg(platform, oldEmoteName, emoteName);

    this->emplace<TimestampElement>();
    if (!actor.isEmpty())
    {
        this->emplace<TextElement>(actor, MessageElementFlag::Username,
                                   MessageColor::System)
            ->setLink({Link::UserInfo, actor});
    }
    this->emplace<TextElement>(text, MessageElementFlag::Text,
                               MessageColor::System);

    QString finalText;
    if (actor.isEmpty())
    {
        finalText = text;
    }
    else
    {
        finalText = QString("%1 %2").arg(actor, text);
    }

    this->message().loginName = actor;
    this->message().messageText = finalText;
    this->message().searchText = finalText;

    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::LiveUpdatesUpdate);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);
}

MessageBuilder::MessageBuilder(LiveUpdatesUpdateEmoteSetMessageTag /*unused*/,
                               const QString &platform, const QString &actor,
                               const QString &emoteSetName)
    : MessageBuilder()
{
    auto text = QString("switched the active %1 Emote Set to \"%2\".")
                    .arg(platform, emoteSetName);

    this->emplace<TimestampElement>();
    this->emplace<TextElement>(actor, MessageElementFlag::Username,
                               MessageColor::System)
        ->setLink({Link::UserInfo, actor});
    this->emplace<TextElement>(text, MessageElementFlag::Text,
                               MessageColor::System);

    auto finalText = QString("%1 %2").arg(actor, text);

    this->message().loginName = actor;
    this->message().messageText = finalText;
    this->message().searchText = finalText;

    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::LiveUpdatesUpdate);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);
}

MessageBuilder::MessageBuilder(ImageUploaderResultTag /*unused*/,
                               const QString &imageLink,
                               const QString &deletionLink,
                               size_t imagesStillQueued, size_t secondsLeft)
    : MessageBuilder()
{
    this->message().flags.set(MessageFlag::System);
    this->message().flags.set(MessageFlag::DoNotTriggerNotification);

    this->emplace<TimestampElement>();

    using MEF = MessageElementFlag;
    auto addText = [this](QString text,
                          MessageColor color =
                              MessageColor::System) -> TextElement * {
        this->message().searchText += text;
        this->message().messageText += text;
        return this->emplace<TextElement>(text, MEF::Text, color);
    };

    addText("Your image has been uploaded to");

    // ASSUMPTION: the user gave this uploader configuration to the program
    // therefore they trust that the host is not wrong/malicious. This doesn't obey getSettings()->lowercaseDomains.
    // This also ensures that the LinkResolver doesn't get these links.
    addText(imageLink, MessageColor::Link)
        ->setLink({Link::Url, imageLink})
        ->setTrailingSpace(!deletionLink.isEmpty());

    if (!deletionLink.isEmpty())
    {
        addText("(Deletion link:");
        addText(deletionLink, MessageColor::Link)
            ->setLink({Link::Url, deletionLink})
            ->setTrailingSpace(false);
        addText(")")->setTrailingSpace(false);
    }
    addText(".");

    if (imagesStillQueued == 0)
    {
        return;
    }

    addText(QString("%1 left. Please wait until all of them are uploaded. "
                    "About %2 seconds left.")
                .arg(imagesStillQueued)
                .arg(secondsLeft));
}

Message *MessageBuilder::operator->()
{
    return this->message_.get();
}

Message &MessageBuilder::message()
{
    return *this->message_;
}

MessagePtr MessageBuilder::release()
{
    std::shared_ptr<Message> ptr;
    this->message_.swap(ptr);
    return ptr;
}

std::weak_ptr<Message> MessageBuilder::weakOf()
{
    return this->message_;
}

void MessageBuilder::append(std::unique_ptr<MessageElement> element)
{
    this->message().elements.push_back(std::move(element));
}

bool MessageBuilder::isEmpty() const
{
    return this->message_->elements.empty();
}

MessageElement &MessageBuilder::back()
{
    assert(!this->isEmpty());
    return *this->message().elements.back();
}

std::unique_ptr<MessageElement> MessageBuilder::releaseBack()
{
    assert(!this->isEmpty());

    auto ptr = std::move(this->message().elements.back());
    this->message().elements.pop_back();
    return ptr;
}

void MessageBuilder::addLink(const linkparser::Parsed &parsedLink,
                             const QString &source)
{
    QString lowercaseLinkString;
    QString origLink = parsedLink.link.toString();
    QString fullUrl;

    if (parsedLink.protocol.isNull())
    {
        fullUrl = QStringLiteral("http://") + origLink;
    }
    else
    {
        lowercaseLinkString += parsedLink.protocol;
        fullUrl = origLink;
    }

    lowercaseLinkString += parsedLink.host.toString().toLower();
    lowercaseLinkString += parsedLink.rest;

    auto textColor = MessageColor(MessageColor::Link);

    if (parsedLink.hasPrefix(source))
    {
        this->emplace<TextElement>(parsedLink.prefix(source).toString(),
                                   MessageElementFlag::Text, this->textColor_)
            ->setTrailingSpace(false);
    }
    auto *el = this->emplace<LinkElement>(
        LinkElement::Parsed{.lowercase = lowercaseLinkString,
                            .original = origLink},
        fullUrl, MessageElementFlag::Text, textColor);
    if (parsedLink.hasSuffix(source))
    {
        el->setTrailingSpace(false);
        this->emplace<TextElement>(parsedLink.suffix(source).toString(),
                                   MessageElementFlag::Text, this->textColor_);
    }

    getApp()->getLinkResolver()->resolve(el->linkInfo());
}

void MessageBuilder::addTextOrEmoji(EmotePtr emote)
{
    this->emplace<EmoteElement>(emote, MessageElementFlag::EmojiAll);
}

void MessageBuilder::addTextOrEmoji(const QString &string)
{
    // Actually just text
    auto link = linkparser::parse(string);
    if (link)
    {
        this->addLink(*link, string);
        return;
    }

    auto &&textColor = this->textColor_;
    if (string.startsWith('@'))
    {
        this->emplace<MentionElement>(string, "", textColor, textColor);
    }
    else
    {
        this->emplace<TextElement>(string, MessageElementFlag::Text, textColor);
    }
}

TextElement *MessageBuilder::emplaceSystemTextAndUpdate(const QString &text,
                                                        QString &toUpdate)
{
    toUpdate.append(text + " ");
    return this->emplace<TextElement>(text, MessageElementFlag::Text,
                                      MessageColor::System);
}

}  // namespace chatterino
