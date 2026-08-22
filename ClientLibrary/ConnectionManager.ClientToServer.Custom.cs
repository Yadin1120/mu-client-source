// <copyright file="ConnectionManager.ClientToServer.Custom.cs" company="MUnique">
// Licensed under the MIT License. See LICENSE file in the project root for full license information.
// </copyright>

namespace MUnique.Client.Library;

using System;
using System.Runtime.InteropServices;
using System.Text;
using MUnique.OpenMU.Network;
using MUnique.OpenMU.Network.Packets.ClientToServer;
using MUnique.OpenMU.Network.Xor;

/// <summary>
/// Extension methods to start writing messages of this namespace on a <see cref="IConnection"/>.
/// </summary>
public unsafe partial class ConnectionManager
{
    private static readonly Xor3Encryptor Xor3Encryptor = new(0);

    /// <summary>
    /// Sends a <see cref="LoginLongPassword" /> to this connection.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    /// <param name="username">The user name, "encrypted" with Xor3.</param>
    /// <param name="password">The password, "encrypted" with Xor3.</param>
    /// <param name="tickCount">The tick count.</param>
    /// <param name="clientVersion">The client version.</param>
    /// <param name="clientSerial">The client serial.</param>
    /// <remarks>
    /// Is sent by the client when: The player tries to log into the game.
    /// Causes reaction on server side: The server is authenticating the sent login name and password. If it's correct, the state of the player is proceeding to be logged in.
    /// </remarks>
    [UnmanagedCallersOnly(EntryPoint = "ConnectionManager_SendLogin")]
    public static void SendLogin(int handle, IntPtr username, IntPtr password, uint @tickCount, byte* @clientVersion, byte* @clientSerial)
    {
        if (!Connections.TryGetValue(handle, out var connection))
        {
            return;
        }

        try
        {
            var usernameStr = NativeInterop.PtrToWideString(@username);
            var passwordStr = NativeInterop.PtrToWideString(@password);

            // todo: check if username or password is too long
            connection.CreateAndSend(pipeWriter =>
            {
                Span<byte> usernameBytes = stackalloc byte[10];
                Span<byte> passwordBytes = stackalloc byte[20];
                Encoding.UTF8.GetBytes(usernameStr, usernameBytes);
                Encoding.UTF8.GetBytes(passwordStr, passwordBytes);
                Xor3Encryptor.Encrypt(usernameBytes);
                Xor3Encryptor.Encrypt(passwordBytes);

                var length = LoginLongPasswordRef.Length;
                var packet = new LoginLongPasswordRef(pipeWriter.GetSpan(length)[..length]);
                usernameBytes.CopyTo(packet.Username);
                passwordBytes.CopyTo(packet.Password);
                packet.TickCount = @tickCount;
                new Span<byte>(@clientVersion, packet.ClientVersion.Length).CopyTo(packet.ClientVersion);
                new Span<byte>(@clientSerial, packet.ClientSerial.Length).CopyTo(packet.ClientSerial);

                return length;
            });
        }
        catch
        {
            // Log exception
        }
    }

    /// <summary>
    /// Sends an <see cref="IncreaseCharacterStatPoint" /> (0xF3, 0x06) with an amount of points
    /// appended after the stat type, so the player can add several points at once from the
    /// character info window. The published packet is 5 bytes long and has no amount field, so
    /// this longer form is written as raw bytes here; the server falls back to a single point
    /// when the packet arrives without the amount.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    /// <param name="statType">The stat type (0 = strength, 1 = agility, 2 = vitality, 3 = energy, 4 = command).</param>
    /// <param name="amount">The number of points to add.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendIncreaseCharacterStatPointMultiple")]
    public static void SendIncreaseCharacterStatPointMultiple(int handle, byte @statType, ushort @amount)
    {
        if (!Connections.TryGetValue(handle, out var connection))
        {
            return;
        }

        try
        {
            connection.CreateAndSend(pipeWriter =>
            {
                const int length = 7;
                var span = pipeWriter.GetSpan(length)[..length];
                span[0] = 0xC1;
                span[1] = length;
                span[2] = 0xF3;
                span[3] = 0x06;
                span[4] = statType;
                span[5] = (byte)(amount >> 8);
                span[6] = (byte)amount;
                return length;
            });
        }
        catch
        {
            // Log exception
        }
    }

    /// <summary>
    /// Sends a MuPassStatusRequest (0xD2, 0x20). This is a custom packet of this server,
    /// not part of the published MUnique.OpenMU.Network.Packets package, so it's written
    /// as raw bytes in this hand-written (non-generated) file.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendMuPassStatusRequest")]
    public static void SendMuPassStatusRequest(int handle)
    {
        SendMuPassSimpleRequest(handle, 0x20);
    }

    /// <summary>
    /// Sends a MuPassCollectRequest (0xD2, 0x21), as raw bytes.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    /// <param name="level">The pass level whose reward is collected.</param>
    /// <param name="proTrack">1 for the pro track, 0 for the free track.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendMuPassCollectRequest")]
    public static void SendMuPassCollectRequest(int handle, byte @level, byte @proTrack)
    {
        if (!Connections.TryGetValue(handle, out var connection))
        {
            return;
        }

        try
        {
            connection.CreateAndSend(pipeWriter =>
            {
                const int length = 6;
                var span = pipeWriter.GetSpan(length)[..length];
                span[0] = 0xC1;
                span[1] = length;
                span[2] = 0xD2;
                span[3] = 0x21;
                span[4] = level;
                span[5] = proTrack;
                return length;
            });
        }
        catch
        {
            // Log exception
        }
    }

    /// <summary>
    /// Sends a MuPassProUpgradeRequest (0xD2, 0x22), as raw bytes.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendMuPassProUpgradeRequest")]
    public static void SendMuPassProUpgradeRequest(int handle)
    {
        SendMuPassSimpleRequest(handle, 0x22);
    }

    /// <summary>
    /// Sends a parameterless MU Pass request packet (0xD2 group) with the given sub-code.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    /// <param name="subCode">The packet sub-code.</param>
    private static void SendMuPassSimpleRequest(int handle, byte subCode)
    {
        if (!Connections.TryGetValue(handle, out var connection))
        {
            return;
        }

        try
        {
            connection.CreateAndSend(pipeWriter =>
            {
                const int length = 4;
                var span = pipeWriter.GetSpan(length)[..length];
                span[0] = 0xC1;
                span[1] = length;
                span[2] = 0xD2;
                span[3] = subCode;
                return length;
            });
        }
        catch
        {
            // Log exception
        }
    }

    /// <summary>
    /// Sends a JewelBankStatusRequest (0xD2, 0x30) - the jewel bank window asks for the balances.
    /// Custom packet of this server, so it's written as raw bytes here.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendJewelBankStatusRequest")]
    public static void SendJewelBankStatusRequest(int handle)
    {
        SendMuPassSimpleRequest(handle, 0x30);
    }

    /// <summary>
    /// Sends a JewelBankDepositRequest (0xD2, 0x31) for one inventory slot.
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    /// <param name="inventorySlot">The inventory slot of the jewel to deposit.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendJewelBankDepositRequest")]
    public static void SendJewelBankDepositRequest(int handle, byte @inventorySlot)
    {
        if (!Connections.TryGetValue(handle, out var connection))
        {
            return;
        }

        try
        {
            connection.CreateAndSend(pipeWriter =>
            {
                const int length = 5;
                var span = pipeWriter.GetSpan(length)[..length];
                span[0] = 0xC1;
                span[1] = length;
                span[2] = 0xD2;
                span[3] = 0x31;
                span[4] = inventorySlot;
                return length;
            });
        }
        catch
        {
            // Log exception
        }
    }

    /// <summary>
    /// Sends a JewelBankWithdrawRequest (0xD2, 0x32).
    /// </summary>
    /// <param name="handle">The handle of the connection.</param>
    /// <param name="jewelType">The number of the jewel mix.</param>
    /// <param name="amount">The number of single jewels; 255 fills every free inventory slot.</param>
    [UnmanagedCallersOnly(EntryPoint = "SendJewelBankWithdrawRequest")]
    public static void SendJewelBankWithdrawRequest(int handle, byte @jewelType, byte @amount)
    {
        if (!Connections.TryGetValue(handle, out var connection))
        {
            return;
        }

        try
        {
            connection.CreateAndSend(pipeWriter =>
            {
                const int length = 6;
                var span = pipeWriter.GetSpan(length)[..length];
                span[0] = 0xC1;
                span[1] = length;
                span[2] = 0xD2;
                span[3] = 0x32;
                span[4] = jewelType;
                span[5] = amount;
                return length;
            });
        }
        catch
        {
            // Log exception
        }
    }
}
